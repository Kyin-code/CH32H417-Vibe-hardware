/*
 * dhcp_packet.h -- DHCPv4 / BOOTP wire format: build and parse.
 *
 * Layering: this file knows nothing about timers, sockets or the state
 * machine. It serialises requests into a caller supplied buffer and decodes
 * replies with hard bounds checking (every length is validated against the
 * bytes actually received; no option may make the parser read past `len`).
 *
 * All multi-byte wire fields are network order; decoded struct fields are
 * host order.
 */
#ifndef DHCP_PACKET_H
#define DHCP_PACKET_H

#include "dhcp_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Constants                                                                 */
/* ------------------------------------------------------------------------- */

#define DHCP_SERVER_PORT 67u
#define DHCP_CLIENT_PORT 68u

#define DHCP_MAGIC_COOKIE 0x63825363u

/* Fixed BOOTP part (RFC 951) preceding the magic cookie. */
#define DHCP_BOOTP_FIXED_LEN 236u
/* Fixed part + cookie: the minimum size of a DHCP message body. */
#define DHCP_MIN_MSG_LEN 240u
/* Enough for a BOOTP header plus one byte of options (a bare END). */

#define DHCP_HLEN_ETHERNET 6u
#define DHCP_HTYPE_ETHERNET 1u

#define DHCP_OP_BOOTREQUEST 1u
#define DHCP_OP_BOOTREPLY 2u

/* flags field */
#define DHCP_FLAG_BROADCAST 0x8000u

/* DHCP message types (option 53) */
#define DHCP_TYPE_DISCOVER 1u
#define DHCP_TYPE_OFFER 2u
#define DHCP_TYPE_REQUEST 3u
#define DHCP_TYPE_DECLINE 4u
#define DHCP_TYPE_ACK 5u
#define DHCP_TYPE_NAK 6u
#define DHCP_TYPE_RELEASE 7u
#define DHCP_TYPE_INFORM 8u

/* DHCP options (RFC 2132) */
#define DHCP_OPT_PAD 0u
#define DHCP_OPT_SUBNET_MASK 1u
#define DHCP_OPT_TIME_OFFSET 2u
#define DHCP_OPT_ROUTER 3u
#define DHCP_OPT_DNS 6u
#define DHCP_OPT_HOSTNAME 12u
#define DHCP_OPT_DOMAIN_NAME 15u
#define DHCP_OPT_BROADCAST_ADDR 28u
#define DHCP_OPT_REQUESTED_IP 50u
#define DHCP_OPT_LEASE_TIME 51u
#define DHCP_OPT_OVERLOAD 52u
#define DHCP_OPT_MSG_TYPE 53u
#define DHCP_OPT_SERVER_ID 54u
#define DHCP_OPT_PARAM_LIST 55u
#define DHCP_OPT_MESSAGE 56u
#define DHCP_OPT_MAX_MSG_SIZE 57u
#define DHCP_OPT_T1 58u
#define DHCP_OPT_T2 59u
#define DHCP_OPT_CLIENT_ID 61u
#define DHCP_OPT_FQDN 81u
#define DHCP_OPT_END 255u

#define DHCP_OPT_MAX_VALUE_LEN 255u

/* Maximum number of option TLVs a single message may contain. */
#define DHCP_MIN_REQ_OPTIONS 3u

/* ------------------------------------------------------------------------- */
/* Decoded message                                                           */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint8_t op;
    uint8_t htype;
    uint8_t hlen;
    uint8_t hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t chaddr[16];

    /* Options we understand. The has_* flags say whether the option was
     * present with a valid length; the value fields are then meaningful. */
    uint8_t msg_type; /* DHCP message type, 0 when absent/invalid */
    uint8_t has_msg_type;

    uint32_t subnet_mask;
    uint8_t has_subnet_mask;

    uint32_t router[DHCP_MAX_ROUTERS];
    uint8_t router_count;

    uint32_t dns[DHCP_MAX_DNS];
    uint8_t dns_count;

    uint32_t server_id;
    uint8_t has_server_id;

    uint32_t lease_time_s;
    uint8_t has_lease_time;

    uint32_t t1_s;
    uint8_t has_t1;

    uint32_t t2_s;
    uint8_t has_t2;

    uint32_t requested_ip; /* option 50, echoed by some servers */
    uint8_t has_requested_ip;

    uint32_t max_msg_size; /* option 57 (server side mostly) */
    uint8_t has_max_msg_size;

    uint8_t has_broadcast_addr;
    uint32_t broadcast_addr;
} dhcp_msg_t;

/* Parse result. */
typedef enum {
    DHCP_PARSE_OK = 0,
    DHCP_PARSE_ERR_NULL_ARG = -1,
    DHCP_PARSE_ERR_TOO_SHORT = -2,   /* shorter than BOOTP fixed part + cookie */
    DHCP_PARSE_ERR_BAD_OP = -3,      /* not a BOOTREPLY                         */
    DHCP_PARSE_ERR_BAD_COOKIE = -4,  /* magic cookie missing (plain BOOTP)      */
    DHCP_PARSE_ERR_BAD_HLEN = -5,    /* hlen > sizeof(chaddr)                   */
    DHCP_PARSE_ERR_OPTIONS = -6,     /* truncated / oversized option (oob risk) */
    DHCP_PARSE_ERR_NO_TYPE = -7      /* DHCP message type option missing        */
} dhcp_parse_result_t;

/* ------------------------------------------------------------------------- */
/* Request construction                                                      */
/* ------------------------------------------------------------------------- */

typedef struct {
    const uint8_t *mac; /* 6 bytes, required */
    uint32_t xid;
    uint16_t secs;  /* elapsed seconds since the client started trying */
    uint16_t flags; /* DHCP_FLAG_BROADCAST as needed */
    uint32_t ciaddr; /* 0 unless renewing/rebinding/releasing */
    uint32_t requested_ip; /* option 50, 0 = omit */
    uint32_t server_id;    /* option 54, 0 = omit */
    const char *hostname;  /* option 12, NULL/empty = omit */
    uint16_t max_msg_size; /* option 57, 0 = omit */
} dhcp_build_params_t;

/**
 * Serialise a client request into `buf`.
 *
 * @param buf    destination buffer, may not be NULL
 * @param cap    capacity of `buf` (usually DHCP_MAX_MSG_PAYLOAD)
 * @param type   DHCP message type to place in option 53
 * @param p      parameters, `p->mac` must point at 6 bytes
 * @return       number of bytes written, or 0 on error (bad args / too small)
 */
uint16_t dhcp_packet_build(uint8_t *buf, uint16_t cap, uint8_t type, const dhcp_build_params_t *p);

/**
 * Decode a received DHCP message.
 *
 * The parser is defensive: `len` is the authoritative size, all option
 * lengths are range checked, values that must have a fixed length (mask,
 * lease time, server id, ...) are rejected when wrong, and malformed option
 * TLVs abort the parse with DHCP_PARSE_ERR_OPTIONS instead of reading out of
 * bounds. Unknown options are skipped.
 */
dhcp_parse_result_t dhcp_packet_parse(const uint8_t *buf, uint16_t len, dhcp_msg_t *out);

/** Human readable names, used by logs and tests. */
const char *dhcp_msg_type_name(uint8_t type);

/** Bytes 236..239 == DHCP_MAGIC_COOKIE ? */
int dhcp_packet_has_magic_cookie(const uint8_t *buf, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* DHCP_PACKET_H */
