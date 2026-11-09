/*
 * dhcp_port.h -- platform adaptation layer.
 *
 * The DHCP core only ever talks to the outside world through this struct.
 * Everything is a function pointer plus one opaque context, so a port needs
 * no globals and several clients/instances can share one process (which is
 * what the host tests do).
 *
 * Only now_ms() and tx_udp() are mandatory.
 */
#ifndef DHCP_PORT_H
#define DHCP_PORT_H

#include "dhcp_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DHCP_LOG_ERROR = 0,
    DHCP_LOG_WARN = 1,
    DHCP_LOG_INFO = 2,
    DHCP_LOG_DEBUG = 3
} dhcp_log_level_t;

/**
 * Platform callbacks.
 *
 * Call context: the core never calls into the port from an ISR expectation --
 * integrators may call dhcp_client_poll()/dhcp_client_input() from their main
 * loop or from an ISR provided the port callbacks are ISR safe.
 */
typedef struct dhcp_port {
    /** Opaque pointer passed back to every callback. */
    void *ctx;

    /**
     * Monotonic millisecond counter, free running, wraps at 2^32.
     * Must never go backwards. Required.
     */
    uint32_t (*now_ms)(void *ctx);

    /**
     * Non-zero pseudo random value. Only used to derive transaction ids and
     * retransmission jitter, so a weak LFSR/xorshift seeded from the clock is
     * good enough. Required.
     */
    uint32_t (*rand32)(void *ctx);

    /**
     * Send one UDP datagram carrying `len` bytes of DHCP payload.
     * The port is responsible for IP/UDP framing (and checksum).
     *
     * @param dst_ip   destination address, host order (255.255.255.255 for
     *                 broadcast).
     * @param dst_port destination UDP port (67 for a server).
     * @return 0 on success, negative on a hard error (buffer busy etc.).
     *         The client retries on failure just like on a lost packet.
     */
    int (*tx_udp)(void *ctx, const uint8_t *data, uint16_t len, uint32_t dst_ip, uint16_t dst_port);

    /** Optional log sink; msg is a NUL terminated string, already formatted. */
    void (*log)(void *ctx, dhcp_log_level_t level, const char *msg);

    /* --- Optional lease persistence (NVM, retained RAM, FRAM, ...) -------- */

    /** Store `len` bytes, replacing any previous blob. 0 = success. */
    int (*lease_save)(void *ctx, const uint8_t *blob, uint16_t len);

    /** Load up to `cap` bytes into `blob`, set *len. 0 = success. */
    int (*lease_load)(void *ctx, uint8_t *blob, uint16_t cap, uint16_t *len);

    /** Erase the stored blob. 0 = success. */
    int (*lease_clear)(void *ctx);
} dhcp_port_t;

/**
 * Statically initialise a port with only the mandatory callbacks present and
 * the rest zeroed; use dhcp_port_set_* helpers.
 */
#define DHCP_PORT_INIT(ctx_, now_ms_, rand32_, tx_udp_) \
    { (ctx_), (now_ms_), (rand32_), (tx_udp_), 0, 0, 0, 0 }

/** Minimal sanity check used by dhcp_client_init(). */
static inline int dhcp_port_is_valid(const dhcp_port_t *p)
{
    return (p != 0) && (p->now_ms != 0) && (p->rand32 != 0) && (p->tx_udp != 0);
}

#ifdef __cplusplus
}
#endif

#endif /* DHCP_PORT_H */
