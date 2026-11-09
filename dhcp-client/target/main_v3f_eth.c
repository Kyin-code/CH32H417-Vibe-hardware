/*
 * main_v3f_eth.c -- CH32H417 V3F application: vendor WCHNET DHCP + our lease
 * management layer.
 *
 * ARCHITECTURE (plan B)
 * ---------------------
 * After several hardware iterations it became clear that a DHCP socket created
 * by hand through WCHNET_SocketCreat() never receives a single frame on this
 * stack: counters showed d=6 DISCOVERs sent, PHY link up, MAC in promiscuous
 * mode, yet RecvRemLen stayed 0 and WCHNET_GetSocketInt() never fired. The
 * vendor's own DHCP works (demo/ETH_DHCP.Bin obtains 192.168.2.73), and
 * disassembling net_dhcp.o shows WCHNET_DHCPStart() is just
 * dhcp_start(&WCHNetIf) -- lwIP's native DHCP client.
 *
 * So we let the vendor path acquire the lease and keep our own value-add:
 *   - WCHNET_DHCPCallBack() receives 20 bytes: ip, gw, mask, dns1, dns2
 *   - we convert that into dhcp_lease_t and run it through lease_apply_policy
 *   - the same event/logging/diagnostics path as before
 *
 * A self-hosted client (dhcp_client.c) is still built and unit-tested; it is
 * simply not wired to the socket here. Flip DHCP_USE_OWN_CLIENT to 1 to go back
 * to it (see README section "known limitations" for why that fails on WCHNET).
 *
 * WHY V3F: demo/ETH_DHCP.Bin keeps every string in the V3F flash window and its
 * V5F window is ~10 KB, i.e. the vendor runs Run_Core_V3FandV5F with Ethernet
 * on V3F.
 *
 * WHY NOT printf: linking newlib printf pulls vfprintf (9.5 KB) plus the
 * soft-float helpers (8.7 KB each) out of a 64 KB flash window.
 */
#include "debug.h"
#include "ch32h417.h"
#include "ch32h417_it.h"
#include "eth_driver.h"
#include "system_ch32h417.h"
#include "wchnet.h"
#include "dhcp_port_wchnet.h"
#include "lease_apply_policy.h"
#include "uprint.h"
#include "http_srv.h"
#include "mdns.h"

#ifndef DHCP_USE_OWN_CLIENT
#define DHCP_USE_OWN_CLIENT 0
#endif

#define APP_HOSTNAME "ch32h417"

/* WCHNET tick period in ms (eth_driver.h: WCHNETTIMERPERIOD == 10). */
#define TICK_MS 10u

static uint8_t s_mac[6];
static applied_netcfg_t s_applied;
static uint8_t s_link_up;
static dhcp_lease_t s_lease;
static volatile uint8_t s_lease_valid;
static http_srv_t s_http;
static mdns_t s_mdns;
static uint8_t s_http_up;
static uint8_t s_mdns_up;
static uint8_t s_net_pending;

#if DHCP_USE_OWN_CLIENT
static dhcp_client_t s_client;
static dhcp_wchnet_ctx_t s_dhcp;
#endif

/* ------------------------------------------------------------------------- */
/* Tick                                                                      */
/* ------------------------------------------------------------------------- */

void app_tim2_tick(void)
{
    WCHNET_TimeIsr(WCHNETTIMERPERIOD);
#if DHCP_USE_OWN_CLIENT
    dhcp_wchnet_tick(&s_dhcp, TICK_MS);
#endif
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
/* Vendor DHCP callback: 20 bytes -> dhcp_lease_t                            */
/* ------------------------------------------------------------------------- */

/*
 * arg layout documented by the vendor example (ETH/DHCP/Common/hardware.c):
 *   p[0..3]   IP address
 *   p[4..7]   gateway
 *   p[8..11]  subnet mask
 *   p[12..15] DNS1
 *   p[16..19] DNS2
 */
static uint8_t on_vendor_dhcp(uint8_t status, void *arg)
{
    const uint8_t *p = (const uint8_t *)arg;

    if (status != 0u || p == 0) {
        uprint_str("DHCP fail status=");
        uprint_hex8(status);
        uprint_str("\n");
        s_lease_valid = 0u;
        return 0u;
    }

    s_lease.address = dhcp_ip4_from_bytes(&p[0]);
    s_lease.router = dhcp_ip4_from_bytes(&p[4]);
    s_lease.subnet_mask = dhcp_ip4_from_bytes(&p[8]);
    s_lease.dns_count = 0u;
    if (dhcp_ip4_from_bytes(&p[12]) != 0u) {
        s_lease.dns[s_lease.dns_count++] = dhcp_ip4_from_bytes(&p[12]);
    }
    if (dhcp_ip4_from_bytes(&p[16]) != 0u && s_lease.dns_count < (uint8_t)DHCP_MAX_DNS) {
        s_lease.dns[s_lease.dns_count++] = dhcp_ip4_from_bytes(&p[16]);
    }
    s_lease.server_id = 0u; /* the vendor callback does not report it */
    s_lease.lease_time_s = 0u;
    s_lease.t1_s = 0u;
    s_lease.t2_s = 0u;
    s_lease.infinite = 0u;
    s_lease.valid = 1u;
    s_lease_valid = 1u;

    uprint_str("DHCP Success\n");
    uprint_str("IPAddr:");
    uprint_ip(s_lease.address);
    uprint_str("\nGWIPAddr:");
    uprint_ip(s_lease.router);
    uprint_str("\nIPMask:");
    uprint_ip(s_lease.subnet_mask);
    uprint_str("\n");
    if (s_lease.dns_count > 0u) {
        uprint_str("DNS1:");
        uprint_ip(s_lease.dns[0]);
        uprint_str("\n");
    }
    if (s_lease.dns_count > 1u) {
        uprint_str("DNS2:");
        uprint_ip(s_lease.dns[1]);
        uprint_str("\n");
    }

    /* Hand it to the same policy code the self-hosted client used. */
    if (lease_apply_decide(&s_applied, &s_lease) != LEASE_APPLY_NONE) {
        lease_apply_commit(&s_applied, &s_lease);
        uprint_str("lease applied (stack already has it)\n");
    }

    /* Do NOT create sockets from here.  This callback runs inside the
     * library's own tick (WCHNET_MainTask -> dhcp_fine_tmr -> dhcp_bind), so
     * calling WCHNET_SocketCreat() re-enters the stack while it is still
     * updating the netif address; pcb allocation fails silently and the
     * socket never shows up.  Just raise a flag and let the main loop start
     * the services on a clean stack (that is also what the vendor examples
     * do - they create sockets in main, never in the DHCP callback). */
    s_net_pending = 1u;

    return 1u;
}

/* ------------------------------------------------------------------------- */
/* Start mDNS + HTTP once a lease is known (main-loop context)                */
/* ------------------------------------------------------------------------- */

static void net_services_poll(void)
{
    if (s_net_pending == 0u || s_lease_valid == 0u) {
        return;
    }
    s_net_pending = 0u;

    if (s_mdns_up == 0u) {
        if (mdns_start(&s_mdns) == 0) {
            s_mdns_up = 1u;
            mdns_set_address(&s_mdns, s_lease.address);
            uprint_str("mdns ok id=");
            uprint_u32(s_mdns.socket_id);
            uprint_str("\n");
        } else {
            uprint_str("mdns FAILED rc=");
            uprint_u32(s_mdns.last_rc);
            uprint_str("\n");
        }
    }

    if (s_http_up == 0u) {
        if (http_start(&s_http, APP_HOSTNAME) == 0) {
            s_http_up = 1u;
            http_set_address(&s_http, s_lease.address);
            uprint_str("http ok id=");
            uprint_u32(s_http.listen_id);
            uprint_str("\n");
            uprint_str("open http://" MDNS_HOST_LABEL ".local/ or http://");
            uprint_ip(s_lease.address);
            uprint_str("/\n");
        } else {
            uprint_str("http FAILED rc=");
            uprint_u32(s_http.last_rc);
            uprint_str("\n");
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Global interrupt                                                          */
/* ------------------------------------------------------------------------- */

static void net_global_int(void)
{
    uint8_t st = WCHNET_GetGlobalInt();
    uint8_t i;

    if ((st & GINT_STAT_PHY_CHANGE) != 0u) {
        uint8_t phy = WCHNET_GetPHYStatus();

        s_link_up = (phy & PHY_LINK_SUCCESS) != 0u ? 1u : 0u;
        uprint_str(s_link_up != 0u ? "PHY Link Success\n" : "PHY link down\n");
    }
    if ((st & GINT_STAT_SOCKET) != 0u) {
        for (i = 0; i < WCHNET_MAX_SOCKET_NUM; i++) {
            /* Sockets owned by the HTTP server and mDNS are driven by their
             * own poll functions; reading the interrupt here would steal the
             * notification from them.  Keying on the source port instead of a
             * cached id matters: a freshly accepted connection is not known to
             * http_poll() yet, but it already carries SourPort 80. */
            if (s_http_up != 0u && http_handles_socket(&s_http, i) != 0) {
                continue;
            }
            if (s_mdns_up != 0u && s_mdns.socket_open != 0u && s_mdns.socket_id == i) {
                continue;
            }
#if DHCP_USE_OWN_CLIENT
            if (dhcp_wchnet_owns_socket(&s_dhcp, i) != 0) {
                continue; /* drained by dhcp_wchnet_poll() */
            }
#endif
            (void)WCHNET_GetSocketInt(i);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Heartbeat                                                                 */
/* ------------------------------------------------------------------------- */

static void heartbeat(void)
{
    static uint32_t last_ms;
    uint32_t now_ms = (uint32_t)g_tim2_ticks * TICK_MS;
    uint8_t k;

    if ((uint32_t)(now_ms - last_ms) < 5000u) {
        return;
    }
    last_ms = now_ms;

    uprint_str("t=");
    uprint_u32(now_ms / 1000u);
    uprint_str("s link=");
    uprint_u32(s_link_up);
    if (s_lease_valid != 0u) {
        uprint_str(" BOUND ip=");
        uprint_ip(s_lease.address);
    } else {
        uprint_str(" acquiring");
    }
#if DHCP_USE_OWN_CLIENT
    {
        const dhcp_stats_t *st = dhcp_client_get_stats(&s_client);

        uprint_str(" own d=");
        uprint_u32(st->tx_discover);
        uprint_str(" off=");
        uprint_u32(st->rx_offer);
        uprint_str(" rem=");
        uprint_u32(SocketInf[s_dhcp.socket_id].RecvRemLen);
    }
#endif
    uprint_str("\n");

    /* Socket table dump: ids, SockStatus, local port.  This is the fastest way
     * to see whether our mDNS/HTTP pcbs were really allocated and in which
     * state they sit. */
    uprint_str("socks:");
    for (k = 0; k < WCHNET_MAX_SOCKET_NUM; k++) {
        uprint_str(" ");
        uprint_u32(k);
        uprint_str("=");
        uprint_u32((uint32_t)SocketInf[k].SockStatus);
        uprint_str("/");
        uprint_u32(SocketInf[k].SourPort);
        uprint_str("r");
        uprint_u32(SocketInf[k].RecvRemLen);
    }
    uprint_str("\n");
}

/* ------------------------------------------------------------------------- */
/* main                                                                      */
/* ------------------------------------------------------------------------- */

int main(void)
{
    uint8_t zero[4] = { 0u, 0u, 0u, 0u };
    uint8_t rc = 0u;
    uint8_t k;

    SystemInit();
    SystemAndCoreClockUpdate();
    Delay_Init();
    uprint_init();
    Delay_Ms(200);

    uprint_str("\n== ch32h417 dhcp-client (V3F) ==\n");
    uprint_str("SystemClk:");
    uprint_u32((uint32_t)SystemCoreClock);
    uprint_str("\n");

    NVIC_WakeUp_V5F(Core_V5F_StartAddr);
    HSEM_FastTake(HSEM_ID0);
    HSEM_ReleaseOneSem(HSEM_ID0, 0);

    WCHNET_GetMacAddr(s_mac);
    uprint_str("mac addr:");
    for (k = 0; k < 6u; k++) {
        uprint_hex8(s_mac[k]);
        uprint_str(k == 5u ? "\n" : " ");
    }

    rc = ETH_LibInit(zero, zero, zero, s_mac);
    uprint_str("net ver:");
    uprint_hex8(WCHNET_GetVer());
    uprint_str("\nWCHNET_LibInit ");
    uprint_str(rc == WCHNET_ERR_SUCCESS ? "Success\n" : "FAILED\n");
    if (rc != WCHNET_ERR_SUCCESS) {
        for (;;) {
            Delay_Ms(1000);
            uprint_str("ethernet init failed\n");
        }
    }

    tim2_init(); /* WCHNET time base */

#if DHCP_USE_OWN_CLIENT
    if (dhcp_wchnet_init(&s_dhcp, &s_client, s_mac, APP_HOSTNAME) != 0) {
        uprint_str("dhcp init FAILED\n");
        for (;;) {
        }
    }
    uprint_str("dhcp start=");
    uprint_u32((uint32_t)dhcp_wchnet_start(&s_dhcp));
    uprint_str("\n");
#else
    (void)WCHNET_DHCPSetHostname(APP_HOSTNAME);
    rc = WCHNET_DHCPStart(on_vendor_dhcp);
    uprint_str("WCHNET_DHCPStart=");
    uprint_u32(rc);
    uprint_str("\n");
#endif

    for (;;) {
#if DHCP_USE_OWN_CLIENT
        dhcp_wchnet_poll(&s_dhcp);
#endif
        WCHNET_MainTask();
        if (WCHNET_QueryGlobalInt()) {
            net_global_int();
        }
        net_services_poll();
        if (s_mdns_up != 0u) {
            mdns_poll(&s_mdns);
        }
        if (s_http_up != 0u) {
            http_poll(&s_http);
        }
#if DHCP_USE_OWN_CLIENT
        dhcp_client_poll(&s_client);
#endif
        heartbeat();
    }
}
