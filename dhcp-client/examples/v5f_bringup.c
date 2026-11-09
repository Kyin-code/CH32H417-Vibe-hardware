/*
 * v5f_bringup.c -- CH32H417 (V5F) integration example for dhcp-client.
 *
 * NOT part of the host build: this file is meant to be dropped into a
 * MounRiver Studio project based on
 *   doc/EVT/EXAM/ETH/DHCP/V5F  (or any other ETH example)
 * with the four files in ../ as extra sources. It is compiled only with the
 * WCH RISC-V toolchain, which is not installed in this workspace, so treat it
 * as a reviewed template rather than a tested binary (see README, "Hardware
 * verification checklist").
 *
 * Why a "two phase boot"?
 * -----------------------
 * The WCH library has no public API to reprogram the interface address after
 * WCHNET_Init(): DHCP is done internally by WCHNET_DHCPStart()/WCHNET_DHCPHandle.
 * Since this client replaces that, there are two practical ways to install a
 * lease (both are shown below):
 *
 *   A. (default here) Keep the lease in a .noinit section and reboot once when
 *      the address changes; the next boot calls ETH_LibInit() with the leased
 *      address. Deterministic, no interaction with library internals.
 *   B. If you have the lwIP headers that libwchnet.a was built against, call
 *      netif_set_addr(&WCHNetIf, ...) directly. libwchnet.a does export
 *      WCHNetIf, netif_set_addr(), netif_set_ipaddr(), netif_set_gw(),
 *      netif_set_netmask() (verified with `nm -g libwchnet.a`), but the EVT
 *      tree ships no lwip/ headers, so B is opt-in and commented out.
 *
 * Verified against the vendor example:
 *   doc/EVT/EXAM/ETH/DHCP/Common/hardware.c   (WCHNET_ETHIsr/MainTask loop,
 *                                              TIM2 -> WCHNET_TimeIsr)
 *   doc/EVT/EXAM/ETH/NetLib/wchnet.h          (socket API)
 *   doc/EVT/EXAM/ETH/NetLib/eth_driver.h      (ETH_LibInit, WCHNET_GetMacAddr)
 */

#include "debug.h"
#include "ch32h417.h"
#include "eth_driver.h"
#include "dhcp_port_wchnet.h"
#include "lease_apply_policy.h"

/* ------------------------------------------------------------------------- */
/* Configuration                                                             */
/* ------------------------------------------------------------------------- */

#define DHCP_EXAMPLE_HOSTNAME "ch32h417"

/* Strategy A: reboot once when the leased address changes. */
#ifndef DHCP_EXAMPLE_REBOOT_ON_LEASE
#define DHCP_EXAMPLE_REBOOT_ON_LEASE 1
#endif

/* ------------------------------------------------------------------------- */
/* Lease persistence in .noinit (survives a software reset)                  */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint8_t blob[DHCP_LEASE_BLOB_LEN];
    uint16_t len;
    uint16_t magic;
} lease_store_t;

#define LEASE_STORE_MAGIC 0xD0C4u

static lease_store_t s_store __attribute__((section(".noinit")));

void dhcp_wchnet_lease_save(const uint8_t *blob, uint16_t len)
{
    if (len > sizeof(s_store.blob)) {
        return;
    }
    memcpy(s_store.blob, blob, len);
    s_store.len = len;
    s_store.magic = LEASE_STORE_MAGIC;
}

int dhcp_wchnet_lease_load(uint8_t *blob, uint16_t cap, uint16_t *len)
{
    if (s_store.magic != LEASE_STORE_MAGIC) {
        return -1;
    }
    if (s_store.len == 0u || s_store.len > cap || s_store.len > sizeof(s_store.blob)) {
        return -1;
    }
    memcpy(blob, s_store.blob, s_store.len);
    *len = s_store.len;
    return 0;
}

void dhcp_wchnet_lease_clear(void)
{
    s_store.magic = 0u;
    s_store.len = 0u;
}

/* ------------------------------------------------------------------------- */
/* Applying the lease                                                        */
/* ------------------------------------------------------------------------- */

static volatile uint8_t s_need_reboot = 0u;
static dhcp_lease_t s_active_lease;
static volatile uint8_t s_have_lease = 0u;

/* What the stack is currently programmed with. Kept in .noinit next to the
 * lease so that, after the reboot below, we still know the address is already
 * applied and do not reboot again on the INIT-REBOOT ACK. */
static applied_netcfg_t s_applied __attribute__((section(".noinit")));

void dhcp_wchnet_apply_lease(const dhcp_lease_t *lease)
{
    lease_apply_action_t action;
    char ip[16];
    char mask[16];
    char gw[16];

    action = lease_apply_decide(&s_applied, lease);

    s_active_lease = *lease;
    s_have_lease = 1u;

    if (action == LEASE_APPLY_NONE) {
        /* Same address/mask/gateway as the running stack: nothing to
         * reprogram. This is what prevents the reboot loop after the
         * INIT-REBOOT ACK also reports DHCP_EVENT_RENEWED. */
        return;
    }

    dhcp_ip4_to_str(lease->address, ip);
    dhcp_ip4_to_str(lease->subnet_mask, mask);
    dhcp_ip4_to_str(lease->router, gw);
    printf("DHCP %s: ip=%s mask=%s gw=%s lease=%us\r\n",
           action == LEASE_APPLY_INITIAL ? "applied" : "changed", ip, mask, gw,
           (unsigned)lease->lease_time_s);

#if DHCP_EXAMPLE_REBOOT_ON_LEASE
    /* Record the configuration *before* resetting, so the next boot starts
     * from a consistent .noinit state. */
    lease_apply_commit(&s_applied, lease);
    s_need_reboot = 1u;
#else
    /* Strategy B (needs the lwIP headers):
     *
     *   ip4_addr_t ip4, mask4, gw4;
     *   ip4_addr_set_u32(&ip4,   lease->address);
     *   ip4_addr_set_u32(&mask4, lease->subnet_mask);
     *   ip4_addr_set_u32(&gw4,   lease->router);
     *   netif_set_addr(&WCHNetIf, &ip4, &mask4, &gw4);
     *   netif_set_up(&WCHNetIf);
     */
    lease_apply_commit(&s_applied, lease);
#endif
}

/* ------------------------------------------------------------------------- */
/* Event log                                                                 */
/* ------------------------------------------------------------------------- */

static void on_dhcp_event(void *user, dhcp_event_t ev, dhcp_state_t old_state,
                          const dhcp_client_t *cl)
{
    (void)user;
    (void)old_state;

    switch (ev) {
    case DHCP_EVENT_BOUND:
    case DHCP_EVENT_RENEWED:
        s_have_lease = 1u;
        break;
    case DHCP_EVENT_LEASE_EXPIRED:
    case DHCP_EVENT_NAK:
    case DHCP_EVENT_FAILED:
        s_have_lease = 0u;
        break;
    case DHCP_EVENT_DECLINED:
    case DHCP_EVENT_RELEASED:
        s_have_lease = 0u;
        break;
    default:
        break;
    }
    printf("dhcp: state=%s event=%d\r\n", dhcp_state_name(dhcp_client_get_state(cl)), (int)ev);
}

/* ------------------------------------------------------------------------- */
/* Timer hook: call from the same periodic ISR that feeds WCHNET_TimeIsr()   */
/* ------------------------------------------------------------------------- */

/*
 * void TIM2_IRQHandler(void)
 * {
 *     WCHNET_TimeIsr(WCHNETTIMERPERIOD);       // 1 ms tick in the EVT examples
 *     dhcp_wchnet_tick(&s_dhcp, WCHNETTIMERPERIOD);
 *     TIM_ClearITPendingBit(TIM2, TIM_IT_Update);
 * }
 */

/* ------------------------------------------------------------------------- */
/* Bring-up                                                                  */
/* ------------------------------------------------------------------------- */

static dhcp_client_t s_client;
static dhcp_wchnet_ctx_t s_dhcp;
static uint8_t s_mac[6];

/* Read a previously stored lease out of .noinit, if any. */
static int load_stored_lease(dhcp_lease_t *out)
{
    uint8_t blob[DHCP_LEASE_BLOB_LEN];
    uint16_t len = 0u;

    if (dhcp_wchnet_lease_load(blob, (uint16_t)sizeof(blob), &len) != 0) {
        return -1;
    }
    if (dhcp_lease_deserialize(blob, len, out) != 0) {
        return -1;
    }
    return 0;
}

void DHCP_Example(void)
{
    uint8_t zero[4] = { 0, 0, 0, 0 };
    dhcp_lease_t stored;
    int have_stored;

    printf("dhcp-client example\r\n");

    WCHNET_GetMacAddr(s_mac);

    /* Phase 2 of the boot: if we still hold a lease, start the stack with it
     * so the address is valid from the first instruction. */
    have_stored = load_stored_lease(&stored);
    if (have_stored == 0) {
        uint8_t ip[4];
        uint8_t gw[4];
        uint8_t mask[4];

        dhcp_ip4_to_bytes(stored.address, ip);
        dhcp_ip4_to_bytes(stored.router, gw);
        dhcp_ip4_to_bytes(stored.subnet_mask, mask);
        printf("restoring lease\r\n");
        (void)ETH_LibInit(ip, gw, mask, s_mac);
        /* The stack now runs on these values: make the policy table agree so
         * the INIT-REBOOT ACK does not request another reboot. */
        lease_apply_commit(&s_applied, &stored);
    } else {
        /* First boot: DHCP runs on an unconfigured interface. */
        (void)ETH_LibInit(zero, zero, zero, s_mac);
        lease_apply_reset(&s_applied);
    }

    if (dhcp_wchnet_init(&s_dhcp, &s_client, s_mac, DHCP_EXAMPLE_HOSTNAME) != 0) {
        printf("dhcp init failed\r\n");
        return;
    }
    dhcp_wchnet_set_event_cb(&s_dhcp, on_dhcp_event, 0);

    if (have_stored == 0) {
        /* Validate what we restored instead of blindly re-DISCOVERing. */
        (void)dhcp_client_restore(&s_client, &stored);
    } else {
        (void)dhcp_wchnet_start(&s_dhcp);
    }

    for (;;) {
        /*
         * IMPORTANT: drain our DHCP socket BEFORE the vendor global-int
         * handler, and make that handler skip it.
         *
         * doc/EVT/EXAM/ETH/DHCP/Common/hardware.c implements
         * WCHNET_HandleGlobalInt() by looping over every socket and calling
         * WCHNET_DataLoopback() on SINT_STAT_RECV. Left as is, it would echo
         * our DHCPOFFER/DHCPACK straight back out of the MAC and swallow it,
         * so the client would never bind (no crash, just "DHCP never works").
         *
         * Patch hardware.c:
         *
         *   if (intstat & GINT_STAT_SOCKET) {
         *       for (i = 0; i < WCHNET_MAX_SOCKET_NUM; i++) {
         *           if (dhcp_wchnet_owns_socket(&s_dhcp, (uint8_t)i)) continue;
         *           socketint = WCHNET_GetSocketInt(i);
         *           if (socketint) WCHNET_HandleSockInt(i, socketint);
         *       }
         *   }
         */
        dhcp_wchnet_poll(&s_dhcp);

        WCHNET_MainTask();
        if (WCHNET_QueryGlobalInt()) {
            WCHNET_HandleGlobalInt();
        }

        dhcp_client_poll(&s_client);

#if DHCP_EXAMPLE_REBOOT_ON_LEASE
        if (s_need_reboot != 0u) {
            s_need_reboot = 0u;
            /* The lease is already in .noinit; the next boot programs the
             * stack with it (see the ETH_LibInit call above). Give the
             * RELEASE-less path a moment so the log can flush. */
            Delay_Ms(200);
            NVIC_SystemReset();
        }
#endif
    }
}
