/*
 * main_v5f.c -- CH32H417 V5F application: WCHNET + own DHCP client.
 *
 * This is the firmware that actually runs the DHCP client on the board:
 *
 *   1. V3F helper (main_v3f.c) wakes the V5F core at 0x00010000 and stops.
 *   2. This core brings up the WCH ethernet library with 0.0.0.0 so DHCP can
 *      run on an unconfigured interface.
 *   3. TIM2 generates the 1 ms tick that feeds both WCHNET_TimeIsr() and
 *      dhcp_wchnet_tick().
 *   4. The main loop drains the DHCP socket, runs the WCHNET task and the
 *      DHCP state machine, and prints the lease over UART8.
 *
 * printf goes to UART8 because Core_V5F selects DEBUG_UART8 in debug.h.
 */
#include "debug.h"
#include "ch32h417.h"
#include "ch32h417_it.h"
#include "system_ch32h417.h"
#include "eth_driver.h"
#include "wchnet.h"
#include "dhcp_port_wchnet.h"
#include "lease_apply_policy.h"

#define APP_HOSTNAME "ch32h417"

/* 0 = only report the lease on the console (safe first bring-up)
 * 1 = also ask the main loop to reboot so the stack can be re-initialised
 *     with the leased address (see dhcp-client/README.md section 7.3). */
#ifndef DHCP_APPLY_MODE
#define DHCP_APPLY_MODE 0
#endif

static dhcp_client_t s_client;
static dhcp_wchnet_ctx_t s_dhcp;
static uint8_t s_mac[6];
static applied_netcfg_t s_applied;
static volatile uint8_t s_want_reboot;

/* ------------------------------------------------------------------------- */
/* Timer: 1 ms tick shared by WCHNET and the DHCP client                     */
/* ------------------------------------------------------------------------- */

void app_tim2_tick(void) /* called from TIM2_IRQHandler */
{
    WCHNET_TimeIsr(WCHNETTIMERPERIOD);
    dhcp_wchnet_tick(&s_dhcp, WCHNETTIMERPERIOD);
}

static void tim2_init(void)
{
    TIM_TimeBaseInitTypeDef t;

    RCC_HB1PeriphClockCmd(RCC_HB1Periph_TIM2, ENABLE);

    TIM_TimeBaseStructInit(&t);
    t.TIM_Period = HCLKClock / 1000000u;
    t.TIM_Prescaler = (uint16_t)(WCHNETTIMERPERIOD * 1000u - 1u);
    t.TIM_ClockDivision = 0;
    t.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM2, &t);
    TIM_ITConfig(TIM2, TIM_IT_Update, ENABLE);
    TIM_Cmd(TIM2, ENABLE);
    TIM_ClearITPendingBit(TIM2, TIM_IT_Update);
    NVIC_EnableIRQ(TIM2_IRQn);
}

/* ------------------------------------------------------------------------- */
/* WCHNET global interrupt: MUST skip the DHCP socket                        */
/* ------------------------------------------------------------------------- */

static void net_global_int(void)
{
    uint8_t st = WCHNET_GetGlobalInt();
    uint8_t i;
    uint8_t s;

    if ((st & GINT_STAT_UNREACH) != 0u) {
        printf("net: unreachable\r\n");
    }
    if ((st & GINT_STAT_IP_CONFLI) != 0u) {
        printf("net: IP conflict!\r\n");
    }
    if ((st & GINT_STAT_PHY_CHANGE) != 0u) {
        uint8_t phy = WCHNET_GetPHYStatus();
        printf("net: phy=0x%02x %s\r\n", phy,
               (phy & PHY_LINK_SUCCESS) != 0u ? "LINK UP" : "link down");
    }
    if ((st & GINT_STAT_SOCKET) != 0u) {
        for (i = 0; i < WCHNET_MAX_SOCKET_NUM; i++) {
            /* The vendor example would call WCHNET_DataLoopback() for every
             * socket here, which would echo our DHCP replies back out and
             * swallow them. Our socket is drained by dhcp_wchnet_poll(). */
            if (dhcp_wchnet_owns_socket(&s_dhcp, i) != 0) {
                continue;
            }
            s = WCHNET_GetSocketInt(i);
            if (s != 0u) {
                /* This firmware has no other sockets. */
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* DHCP reporting                                                            */
/* ------------------------------------------------------------------------- */

static void print_ip(const char *tag, uint32_t ip)
{
    char buf[16];
    dhcp_ip4_to_str(ip, buf);
    printf("%s=%s", tag, buf);
}

static void on_dhcp_event(void *user, dhcp_event_t ev, dhcp_state_t old_state,
                          const dhcp_client_t *cl)
{
    const dhcp_lease_t *l = dhcp_client_get_lease(cl);

    (void)user;
    (void)old_state;

    switch (ev) {
    case DHCP_EVENT_BOUND:
    case DHCP_EVENT_RENEWED:
        printf("\r\ndhcp: %s\r\n", ev == DHCP_EVENT_BOUND ? "BOUND" : "RENEWED");
        print_ip("  ip  ", l->address);
        print_ip("  mask", l->subnet_mask);
        print_ip("  gw  ", l->router);
        printf("\r\n");
        if (l->dns_count > 0u) {
            print_ip("  dns1", l->dns[0]);
            printf("\r\n");
        }
        print_ip("  srv ", l->server_id);
        printf("\r\n  lease=%us t1=%us t2=%us\r\n", (unsigned)l->lease_time_s,
               (unsigned)l->t1_s, (unsigned)l->t2_s);
        break;
    case DHCP_EVENT_NAK:
        printf("dhcp: NAK, restarting\r\n");
        break;
    case DHCP_EVENT_LEASE_EXPIRED:
        printf("dhcp: lease expired\r\n");
        break;
    case DHCP_EVENT_FAILED:
        printf("dhcp: giving up, will retry\r\n");
        break;
    case DHCP_EVENT_DECLINED:
        printf("dhcp: address declined\r\n");
        break;
    case DHCP_EVENT_RELEASED:
        printf("dhcp: released\r\n");
        break;
    default:
        break;
    }
}

/*
 * Called by the port whenever a lease is bound or renewed. In mode 0 we only
 * log it; in mode 1 the main loop reboots once so the stack can start with the
 * leased address (the WCH library has no public "set my IP" call).
 */
void dhcp_wchnet_apply_lease(const dhcp_lease_t *lease)
{
    lease_apply_action_t action = lease_apply_decide(&s_applied, lease);

    if (action == LEASE_APPLY_NONE) {
        return;
    }
    printf("dhcp: apply lease (%s)\r\n", action == LEASE_APPLY_INITIAL ? "initial" : "changed");
    lease_apply_commit(&s_applied, lease);
#if DHCP_APPLY_MODE == 1
    s_want_reboot = 1u;
#endif
}

/* ------------------------------------------------------------------------- */
/* Heartbeat                                                                 */
/* ------------------------------------------------------------------------- */

static void heartbeat(void)
{
    static uint32_t last;
    uint32_t now = g_tim2_ticks;
    const dhcp_client_t *cl = &s_client;
    const dhcp_stats_t *st = dhcp_client_get_stats(cl);
    const dhcp_lease_t *l = dhcp_client_get_lease(cl);

    if ((uint32_t)(now - last) < 5000u) {
        return;
    }
    last = now;

    printf("t=%us %s", (unsigned)(now / 1000u), dhcp_state_name(dhcp_client_get_state(cl)));
    if (l->valid != 0u) {
        print_ip(" ip", l->address);
    }
    printf(" | d=%u r=%u off=%u ack=%u nak=%u bad=%u ign=%u\r\n", (unsigned)st->tx_discover,
           (unsigned)st->tx_request, (unsigned)st->rx_offer, (unsigned)st->rx_ack,
           (unsigned)st->rx_nak, (unsigned)st->rx_malformed, (unsigned)st->rx_ignored);
}

/* ------------------------------------------------------------------------- */
/* main                                                                      */
/* ------------------------------------------------------------------------- */

int main(void)
{
    uint8_t zero[4] = { 0u, 0u, 0u, 0u };
    uint8_t rc;

    SystemAndCoreClockUpdate();
    Delay_Init();
    USART_Printf_Init(115200);

    printf("\r\n\r\n=== ch32h417 DHCP client (V5F) ===\r\n");
    printf("SystemClk=%u CoreClk=%u\r\n", (unsigned)SystemClock, (unsigned)SystemCoreClock);

    WCHNET_GetMacAddr(s_mac);
    printf("mac=%02x:%02x:%02x:%02x:%02x:%02x\r\n", s_mac[0], s_mac[1], s_mac[2], s_mac[3],
           s_mac[4], s_mac[5]);

    /* DHCP runs on an unconfigured interface: source 0.0.0.0, broadcast. */
    rc = ETH_LibInit(zero, zero, zero, s_mac);
    printf("ETH_LibInit=0x%02x netver=0x%02x %s\r\n", rc, WCHNET_GetVer(),
           rc == WCHNET_ERR_SUCCESS ? "ok" : "FAILED");
    if (rc != WCHNET_ERR_SUCCESS) {
        printf("ethernet init failed, halting\r\n");
        while (1) {
        }
    }
    printf("net stack ready\r\n");

    if (dhcp_wchnet_init(&s_dhcp, &s_client, s_mac, APP_HOSTNAME) != 0) {
        printf("dhcp init failed, halting\r\n");
        while (1) {
        }
    }
    dhcp_wchnet_set_event_cb(&s_dhcp, on_dhcp_event, 0);

    tim2_init(); /* starts WCHNET_TimeIsr + dhcp tick */

    rc = dhcp_wchnet_start(&s_dhcp);
    printf("dhcp start=%d state=%s\r\n", (int)rc, dhcp_state_name(dhcp_client_get_state(&s_client)));

    for (;;) {
        /* Drain the DHCP socket first: the global-int handler must not touch it. */
        dhcp_wchnet_poll(&s_dhcp);

        WCHNET_MainTask();
        if (WCHNET_QueryGlobalInt()) {
            net_global_int();
        }

        dhcp_client_poll(&s_client);
        heartbeat();

#if DHCP_APPLY_MODE == 1
        if (s_want_reboot != 0u) {
            s_want_reboot = 0u;
            printf("dhcp: rebooting to program stack with leased address\r\n");
            Delay_Ms(100);
            NVIC_SystemReset();
        }
#endif
    }
}
