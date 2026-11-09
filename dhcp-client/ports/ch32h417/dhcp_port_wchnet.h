/*
 * dhcp_port_wchnet.h -- CH32H417 (WCHNET / ETH library) adaptation layer.
 *
 * This port only depends on the public WCHNET API (doc/EVT/EXAM/ETH/NetLib/
 * wchnet.h) and on ETH_LibInit() from eth_driver.c. It does NOT use the
 * vendor's built-in DHCP (WCHNET_DHCPStart): this client replaces it.
 *
 * Transport: by default a UDP socket bound to port 68 that sends with
 * WCHNET_SocketUdpSendTo(). With DHCP_WCHNET_USE_UDP_SOCKET=0 an IP_RAW socket
 * (protocol 17) is used instead and the UDP header is built here, which is a
 * useful fallback on stacks/socket configurations that reject UDP broadcasts
 * from an unconfigured interface.
 *
 * Time base: call dhcp_wchnet_tick(ctx, elapsed_ms) from a periodic timer
 * interrupt (the same timer that calls WCHNET_TimeIsr()).
 * Receive path: call dhcp_wchnet_poll(ctx) from the main loop, next to
 * WCHNET_MainTask() / WCHNET_HandleGlobalInt().
 */
#ifndef DHCP_PORT_WCHNET_H
#define DHCP_PORT_WCHNET_H

#include "dhcp_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Set to 0 to use an IP_RAW socket instead of a UDP socket. */
#ifndef DHCP_WCHNET_USE_UDP_SOCKET
#define DHCP_WCHNET_USE_UDP_SOCKET 1
#endif

/* Receive staging buffer for one DHCP datagram. */
#ifndef DHCP_WCHNET_RX_BUF_LEN
#define DHCP_WCHNET_RX_BUF_LEN 600
#endif

/* Socket ids handed out by WCHNET_SocketCreat() fit in uint8_t. */
typedef struct dhcp_wchnet_ctx {
    uint8_t socket_id;
    uint8_t socket_open;
    /* Written only by dhcp_wchnet_tick() (timer ISR), read by the main loop
     * through wchnet_now_ms(). volatile keeps the compiler from caching it in
     * a register across the polling loop.
     *
     * Race analysis: both sides use aligned 32-bit accesses, which are atomic
     * on CH32H417. The ISR only does "now += ms" (read-modify-write) and the
     * main loop only reads, so the reader can at worst observe a value that is
     * one tick old -- never a torn value. If you want to be defensive, or if
     * another core (V3F) ever writes this field, wrap the += and the read in a
     * short interrupt-disable critical section (see DHCP_WCHNET_TICK_CRITICAL
     * in dhcp_port_wchnet.c).
     */
    volatile uint32_t now_ms;
    uint32_t rng;
    uint8_t rx_buf[DHCP_WCHNET_RX_BUF_LEN];
    uint8_t dst_ip[4]; /* current socket destination (IP_RAW mode only) */
    /* Diagnostics: what the WCHNET socket layer reports. */
    uint8_t last_intstat;   /* last value from WCHNET_GetSocketInt()  */
    uint32_t rx_int_total;  /* how often that was non-zero            */
    uint32_t rx_frames;     /* non-empty frames pulled out of the socket */
    uint32_t rx_oversize;   /* datagrams dropped for being too large   */
    dhcp_client_t *client;
    dhcp_event_cb_t user_cb;
    void *user;
    dhcp_port_t port;
} dhcp_wchnet_ctx_t;

/**
 * Wire the port up and initialise the DHCP client.
 *
 * Prerequisites: ETH_LibInit() has been called (the WCHNET stack is running)
 * and WCHNET_DHCPStart() is NOT used. Passing 0.0.0.0 as the stack address
 * during the first boot is fine and expected: DHCP runs on an unconfigured
 * interface.
 *
 * @return 0 on success, negative on error.
 */
int dhcp_wchnet_init(dhcp_wchnet_ctx_t *ctx, dhcp_client_t *cl, const uint8_t mac[6],
                     const char *hostname);

/** Create the socket and start DISCOVER. 0 on success. */
int dhcp_wchnet_start(dhcp_wchnet_ctx_t *ctx);

/** Advance the millisecond time base. Call from a periodic timer ISR. */
void dhcp_wchnet_tick(dhcp_wchnet_ctx_t *ctx, uint32_t ms);

/** Drain received DHCP datagrams into the client. Call from the main loop. */
void dhcp_wchnet_poll(dhcp_wchnet_ctx_t *ctx);

/** Install an application event callback (in addition to lease applying). */
void dhcp_wchnet_set_event_cb(dhcp_wchnet_ctx_t *ctx, dhcp_event_cb_t cb, void *user);

/**
 * Implement this in the application: it is called whenever a lease is bound or
 * renewed and must program the address into the network stack.
 *
 * A weak default is provided that only records the values, so the project
 * still links before the integrator implements it (see
 * examples/v5f_bringup.c for two working strategies).
 */
void dhcp_wchnet_apply_lease(const dhcp_lease_t *lease);

/** Lease persistence hooks (backup registers / flash). Weak, off by default. */
void dhcp_wchnet_lease_save(const uint8_t *blob, uint16_t len);
int dhcp_wchnet_lease_load(uint8_t *blob, uint16_t cap, uint16_t *len);
void dhcp_wchnet_lease_clear(void);

/**
 * True when `sockid` is the DHCP socket owned by this port.
 *
 * Needed because the vendor example's WCHNET_HandleGlobalInt() loops over all
 * sockets and calls WCHNET_DataLoopback() on SINT_STAT_RECV -- which would
 * echo our DHCP OFFER/ACK straight back out and swallow it. Skip this socket:
 *
 *     for (i = 0; i < WCHNET_MAX_SOCKET_NUM; i++) {
 *         if (dhcp_wchnet_owns_socket(&s_dhcp, (uint8_t)i)) continue;
 *         socketint = WCHNET_GetSocketInt(i);
 *         ...
 *     }
 */
int dhcp_wchnet_owns_socket(const dhcp_wchnet_ctx_t *ctx, uint8_t sockid);

/** Convenience: read the current lease, NULL when not bound. */
const dhcp_lease_t *dhcp_wchnet_lease(const dhcp_wchnet_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* DHCP_PORT_WCHNET_H */
