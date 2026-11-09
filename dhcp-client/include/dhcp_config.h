/*
 * dhcp_config.h -- compile-time tuning and small IPv4/time helpers.
 *
 * Part of dhcp-client: a portable, host-testable DHCPv4 client for embedded
 * targets (CH32H417 / WCHNET and anything else with a UDP-ish transport).
 *
 * No dynamic memory, no floating point, no libc dependency (C99).
 */
#ifndef DHCP_CONFIG_H
#define DHCP_CONFIG_H

#include <stdint.h>
#include <stddef.h>

#define DHCP_CLIENT_VERSION_MAJOR 1
#define DHCP_CLIENT_VERSION_MINOR 0
#define DHCP_CLIENT_VERSION_STRING "1.0.0"

/* ------------------------------------------------------------------------- */
/* Feature switches                                                          */
/* ------------------------------------------------------------------------- */

/* Send DHCP option 12 (host name) when cfg.hostname is not empty. */
#ifndef DHCP_ENABLE_HOSTNAME
#define DHCP_ENABLE_HOSTNAME 1
#endif

/* Allow the client to persist/restore a lease through the port layer. */
#ifndef DHCP_ENABLE_PERSISTENCE
#define DHCP_ENABLE_PERSISTENCE 1
#endif

/* Allow the optional port->log() callback to be used. */
#ifndef DHCP_ENABLE_LOG
#define DHCP_ENABLE_LOG 1
#endif

/* Validate that replies echo our xid / MAC / magic cookie (RFC 2131).
 * Do not disable on production code; it exists so fuzz builds can be lenient. */
#ifndef DHCP_STRICT_REPLY_CHECK
#define DHCP_STRICT_REPLY_CHECK 1
#endif

/* ------------------------------------------------------------------------- */
/* Sizing                                                                    */
/* ------------------------------------------------------------------------- */

/* RFC 2132 section 9.10: a client that cannot reassemble larger messages
 * advertises 576 and must accept at least that. Ethernet MTU 1500 would allow
 * more, but 576 keeps the TX/RX scratch small on MCUs. */
#define DHCP_MAX_MSG_SIZE 576u

/* Useful payload budget: 576 - 20 (IPv4) - 8 (UDP). */
#define DHCP_MAX_MSG_PAYLOAD (DHCP_MAX_MSG_SIZE - 28u)

#define DHCP_MAX_PAYLOAD_BYTES DHCP_MAX_MSG_PAYLOAD

#ifndef DHCP_MAX_DNS
#define DHCP_MAX_DNS 2
#endif

#ifndef DHCP_MAX_ROUTERS
#define DHCP_MAX_ROUTERS 2
#endif

#ifndef DHCP_HOSTNAME_MAX
#define DHCP_HOSTNAME_MAX 32
#endif

#ifdef DHCP_STATIC_ASSERT
#error "DHCP_STATIC_ASSERT is reserved"
#endif

/* ------------------------------------------------------------------------- */
/* Defaults (milliseconds unless stated otherwise)                           */
/* ------------------------------------------------------------------------- */

/* RFC 2131 section 4.1: first retransmission ~4s, then exponential, with
 * jitter. We use 4s -> 8s -> 16s -> 32s -> 64s (capped). */
#define DHCP_DEFAULT_RETRY_BASE_MS 4000u
#define DHCP_DEFAULT_RETRY_MAX_MS 64000u
#define DHCP_DEFAULT_MAX_RETRIES 6u

/* How long we keep collecting DHCPOFFERs before committing to one. */
#define DHCP_DEFAULT_OFFER_COLLECT_MS 1000u

/* RFC 2131 section 3.1.5: wait 10s before restarting after DHCPDECLINE. */
#define DHCP_DEFAULT_DECLINE_WAIT_MS 10000u

/* RFC 2131 section 4.4.5: wait before restarting after a DHCPNAK. */
#define DHCP_DEFAULT_NAK_WAIT_MS 10000u

/* Lease fraction defaults, in permille: T1 = 50%, T2 = 87.5%. */
#define DHCP_DEFAULT_T1_PERMILLE 500u
#define DHCP_DEFAULT_T2_PERMILLE 875u

/* Infinite lease marker (option 51 == 0xffffffff). */
#define DHCP_INFINITE_LEASE_S 0xFFFFFFFFu
#define DHCP_INFINITE_LEASE_MS 0xFFFFFFFFu

/* NOTE: there is deliberately no minimum lease time policy. Any
 * lease_time_s > 0 is accepted; a short lease simply results in short T1/T2
 * windows. lease_time_s == 0 is rejected as malformed (RFC 2132 requires the
 * server to send a usable value), and a missing option 51 falls back to one
 * hour so the client can never end up with an unbounded lease. */

/* Client identifier: type 1 (Ethernet) + MAC. */
#define DHCP_CLIENT_ID_HTYPE_ETH 1u

/* ------------------------------------------------------------------------- */
/* Tiny helpers                                                              */
/* ------------------------------------------------------------------------- */

/** Build a host-order IPv4 address value from four octets. */
static inline uint32_t dhcp_ip4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d;
}

/** Bytes (network order, as on the wire) -> host-order value. */
static inline uint32_t dhcp_ip4_from_bytes(const uint8_t b[4])
{
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

/** Host-order value -> bytes in network order. out may alias nothing. */
static inline void dhcp_ip4_to_bytes(uint32_t ip, uint8_t out[4])
{
    out[0] = (uint8_t)(ip >> 24);
    out[1] = (uint8_t)(ip >> 16);
    out[2] = (uint8_t)(ip >> 8);
    out[3] = (uint8_t)ip;
}

static inline int dhcp_ip4_is_zero(uint32_t ip)
{
    return ip == 0u;
}

/* 255.255.255.255 */
#define DHCP_IP4_BROADCAST 0xFFFFFFFFu
#define DHCP_IP4_ANY 0x00000000u

/** Write dotted-quad text (no snprintf needed). out must hold >= 16 bytes. */
static inline void dhcp_ip4_to_str(uint32_t ip, char *out)
{
    uint8_t o[4];
    int i;
    char *p = out;

    dhcp_ip4_to_bytes(ip, o);
    for (i = 0; i < 4; i++) {
        unsigned v = o[i];
        if (v >= 100u) {
            *p++ = (char)('0' + (v / 100u));
            v %= 100u;
            *p++ = (char)('0' + (v / 10u));
            *p++ = (char)('0' + (v % 10u));
        } else if (v >= 10u) {
            *p++ = (char)('0' + (v / 10u));
            *p++ = (char)('0' + (v % 10u));
        } else {
            *p++ = (char)('0' + v);
        }
        if (i != 3) {
            *p++ = '.';
        }
    }
    *p = '\0';
}

/* Wrap-safe 32-bit millisecond arithmetic (works across the 49.7 day wrap). */
static inline int dhcp_time_reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static inline uint32_t dhcp_time_diff(uint32_t now, uint32_t then)
{
    return (uint32_t)(now - then);
}

static inline uint32_t dhcp_time_add(uint32_t base, uint32_t delta)
{
    return (uint32_t)(base + delta);
}

#endif /* DHCP_CONFIG_H */
