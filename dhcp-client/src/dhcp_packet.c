/*
 * dhcp_packet.c -- DHCPv4 wire format, build + bounds-checked parse.
 *
 * Everything here is pure functions over byte buffers: no globals, no I/O, no
 * allocation. The parser treats `len` as authoritative and never reads or
 * writes outside the caller's buffer, even for hostile input.
 */
#include "dhcp_packet.h"

#include <string.h>

/* Minimum DHCP message length we pad requests to (RFC 1542 BOOTP relay rule
 * of 300 bytes; some servers/relays are picky about shorter frames). */
#define DHCP_MIN_TX_PAD 300u

/* ------------------------------------------------------------------------- */
/* Write cursor                                                              */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint8_t *buf;
    uint16_t cap;
    uint16_t pos;
    uint8_t ok; /* sticky failure flag */
} wcur_t;

static void w_u8(wcur_t *w, uint8_t v)
{
    if (!w->ok || w->pos >= w->cap) {
        w->ok = 0;
        return;
    }
    w->buf[w->pos++] = v;
}

static void w_u16(wcur_t *w, uint16_t v)
{
    w_u8(w, (uint8_t)(v >> 8));
    w_u8(w, (uint8_t)(v & 0xFFu));
}

static void w_u32(wcur_t *w, uint32_t v)
{
    w_u8(w, (uint8_t)(v >> 24));
    w_u8(w, (uint8_t)(v >> 16));
    w_u8(w, (uint8_t)(v >> 8));
    w_u8(w, (uint8_t)v);
}

static void w_zero(wcur_t *w, uint16_t n)
{
    uint16_t i;
    for (i = 0; i < n; i++) {
        w_u8(w, 0u);
    }
}

static void w_bytes(wcur_t *w, const uint8_t *src, uint16_t n)
{
    uint16_t i;
    for (i = 0; i < n; i++) {
        w_u8(w, src[i]);
    }
}

/* Reserve an option header, returns the offset of the length byte. */
static uint16_t opt_open(wcur_t *w, uint8_t code)
{
    uint16_t lenpos;

    if (!w->ok || (uint32_t)w->pos + 2u > (uint32_t)w->cap) {
        w->ok = 0;
        return 0u;
    }
    w->buf[w->pos++] = code;
    lenpos = w->pos++;
    return lenpos;
}

static void opt_close(wcur_t *w, uint16_t lenpos)
{
    uint16_t olen;

    if (!w->ok) {
        return;
    }
    olen = (uint16_t)(w->pos - lenpos - 1u);
    if (olen > DHCP_OPT_MAX_VALUE_LEN) {
        w->ok = 0;
        return;
    }
    w->buf[lenpos] = (uint8_t)olen;
}

static void opt_u8(wcur_t *w, uint8_t code, uint8_t value)
{
    uint16_t lp = opt_open(w, code);
    w_u8(w, value);
    opt_close(w, lp);
}

static void opt_u16(wcur_t *w, uint8_t code, uint16_t value)
{
    uint16_t lp = opt_open(w, code);
    w_u16(w, value);
    opt_close(w, lp);
}

static void opt_u32(wcur_t *w, uint8_t code, uint32_t value)
{
    uint16_t lp = opt_open(w, code);
    w_u32(w, value);
    opt_close(w, lp);
}

/* ------------------------------------------------------------------------- */
/* Build                                                                     */
/* ------------------------------------------------------------------------- */

uint16_t dhcp_packet_build(uint8_t *buf, uint16_t cap, uint8_t type, const dhcp_build_params_t *p)
{
    wcur_t w;
    uint16_t lp;
    static const uint8_t param_req[] = {
        DHCP_OPT_SUBNET_MASK, DHCP_OPT_ROUTER,      DHCP_OPT_DNS,
        DHCP_OPT_LEASE_TIME,  DHCP_OPT_SERVER_ID,   DHCP_OPT_T1,
        DHCP_OPT_T2,          DHCP_OPT_BROADCAST_ADDR
    };

    if (buf == 0 || p == 0 || p->mac == 0 || cap < DHCP_MIN_MSG_LEN) {
        return 0u;
    }
    if (type < DHCP_TYPE_DISCOVER || type > DHCP_TYPE_INFORM) {
        return 0u;
    }

    w.buf = buf;
    w.cap = cap;
    w.pos = 0u;
    w.ok = 1u;

    /* --- fixed BOOTP header (RFC 951 / RFC 2131 table 1) --- */
    w_u8(&w, DHCP_OP_BOOTREQUEST);
    w_u8(&w, DHCP_HTYPE_ETHERNET);
    w_u8(&w, DHCP_HLEN_ETHERNET);
    w_u8(&w, 0u);            /* hops */
    w_u32(&w, p->xid);       /* transaction id */
    w_u16(&w, p->secs);      /* seconds since the attempt started */
    w_u16(&w, p->flags);     /* broadcast flag */
    w_u32(&w, p->ciaddr);    /* client address when renewing/rebinding */
    w_u32(&w, 0u);           /* yiaddr */
    w_u32(&w, 0u);           /* siaddr */
    w_u32(&w, 0u);           /* giaddr */
    w_bytes(&w, p->mac, 6u); /* chaddr[0..5] */
    w_zero(&w, 10u);         /* chaddr[6..15] */
    w_zero(&w, 64u);         /* sname */
    w_zero(&w, 128u);        /* file */

    /* --- magic cookie --- */
    w_u32(&w, DHCP_MAGIC_COOKIE);

    /* --- options: message type first (RFC 2131 section 4.1) --- */
    opt_u8(&w, DHCP_OPT_MSG_TYPE, type);

    /* client identifier: type 1 (Ethernet) + MAC, stable across reboots so a
     * server can hand back the same address after a restart. */
    lp = opt_open(&w, DHCP_OPT_CLIENT_ID);
    w_u8(&w, DHCP_CLIENT_ID_HTYPE_ETH);
    w_bytes(&w, p->mac, 6u);
    opt_close(&w, lp);

#if DHCP_ENABLE_HOSTNAME
    if (p->hostname != 0 && p->hostname[0] != '\0') {
        uint16_t n = 0u;
        uint16_t hp = opt_open(&w, DHCP_OPT_HOSTNAME);
        while (p->hostname[n] != '\0' && n < DHCP_OPT_MAX_VALUE_LEN) {
            w_u8(&w, (uint8_t)p->hostname[n]);
            n++;
        }
        opt_close(&w, hp);
    }
#else
    (void)p->hostname;
#endif

    if (p->max_msg_size != 0u) {
        opt_u16(&w, DHCP_OPT_MAX_MSG_SIZE, p->max_msg_size);
    }

    /* Parameter request list: keep it short. */
    lp = opt_open(&w, DHCP_OPT_PARAM_LIST);
    w_bytes(&w, param_req, (uint16_t)sizeof(param_req));
    opt_close(&w, lp);

    /* Options that are only valid for some message types. */
    if (p->requested_ip != 0u &&
        (type == DHCP_TYPE_REQUEST || type == DHCP_TYPE_DECLINE)) {
        opt_u32(&w, DHCP_OPT_REQUESTED_IP, p->requested_ip);
    }
    if (p->server_id != 0u &&
        (type == DHCP_TYPE_REQUEST || type == DHCP_TYPE_DECLINE ||
         type == DHCP_TYPE_RELEASE)) {
        opt_u32(&w, DHCP_OPT_SERVER_ID, p->server_id);
    }

    w_u8(&w, DHCP_OPT_END);

    if (!w.ok) {
        return 0u;
    }

    /* Pad short requests: relays and some servers dislike tiny DHCP frames. */
    while (w.pos < DHCP_MIN_TX_PAD && w.pos < w.cap) {
        buf[w.pos++] = DHCP_OPT_PAD;
    }

    return w.pos;
}

/* ------------------------------------------------------------------------- */
/* Parse                                                                     */
/* ------------------------------------------------------------------------- */

static uint16_t rd_u16(const uint8_t *b)
{
    return (uint16_t)(((uint16_t)b[0] << 8) | (uint16_t)b[1]);
}

static uint32_t rd_u32(const uint8_t *b)
{
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

int dhcp_packet_has_magic_cookie(const uint8_t *buf, uint16_t len)
{
    if (buf == 0 || len < DHCP_MIN_MSG_LEN) {
        return 0;
    }
    return rd_u32(&buf[DHCP_BOOTP_FIXED_LEN]) == DHCP_MAGIC_COOKIE;
}

/* Store up to cap values of a length-delimited, 4-byte-per-value option. */
static uint8_t take_ip_list(const uint8_t *val, uint8_t vlen, uint32_t *out, uint8_t cap)
{
    uint8_t n = 0u;
    uint8_t i;

    if ((vlen & 3u) != 0u) {
        return 0u; /* malformed length: ignore the whole option */
    }
    for (i = 0; (uint8_t)(i + 4u) <= vlen && n < cap; i = (uint8_t)(i + 4u)) {
        out[n++] = rd_u32(&val[i]);
    }
    return n;
}

dhcp_parse_result_t dhcp_packet_parse(const uint8_t *buf, uint16_t len, dhcp_msg_t *out)
{
    uint16_t i;

    if (buf == 0 || out == 0) {
        return DHCP_PARSE_ERR_NULL_ARG;
    }
    if (len < DHCP_MIN_MSG_LEN) {
        return DHCP_PARSE_ERR_TOO_SHORT;
    }
    memset(out, 0, sizeof(*out));

    out->op = buf[0];
    out->htype = buf[1];
    out->hlen = buf[2];
    out->hops = buf[3];
    if (out->op != DHCP_OP_BOOTREQUEST && out->op != DHCP_OP_BOOTREPLY) {
        return DHCP_PARSE_ERR_BAD_OP;
    }
    if (out->hlen > (uint8_t)sizeof(out->chaddr)) {
        return DHCP_PARSE_ERR_BAD_HLEN;
    }

    out->xid = rd_u32(&buf[4]);
    out->secs = rd_u16(&buf[8]);
    out->flags = rd_u16(&buf[10]);
    out->ciaddr = rd_u32(&buf[12]);
    out->yiaddr = rd_u32(&buf[16]);
    out->siaddr = rd_u32(&buf[20]);
    out->giaddr = rd_u32(&buf[24]);
    memcpy(out->chaddr, &buf[28], sizeof(out->chaddr));

    if (rd_u32(&buf[DHCP_BOOTP_FIXED_LEN]) != DHCP_MAGIC_COOKIE) {
        return DHCP_PARSE_ERR_BAD_COOKIE;
    }

    /* --- options: TLV walk with strict bounds checking ------------------ */
    i = (uint16_t)DHCP_MIN_MSG_LEN;
    while (i < len) {
        uint8_t code = buf[i++];
        uint8_t vlen;
        const uint8_t *val;

        if (code == DHCP_OPT_PAD) {
            continue;
        }
        if (code == DHCP_OPT_END) {
            break;
        }
        if (i >= len) {
            /* length byte missing */
            return DHCP_PARSE_ERR_OPTIONS;
        }
        vlen = buf[i++];
        if ((uint32_t)i + (uint32_t)vlen > (uint32_t)len) {
            /* option claims more bytes than were received */
            return DHCP_PARSE_ERR_OPTIONS;
        }
        val = &buf[i];

        switch (code) {
        case DHCP_OPT_MSG_TYPE:
            if (vlen == 1u) {
                out->msg_type = val[0];
                out->has_msg_type = 1u;
            }
            break;
        case DHCP_OPT_SUBNET_MASK:
            if (vlen == 4u) {
                out->subnet_mask = rd_u32(val);
                out->has_subnet_mask = 1u;
            }
            break;
        case DHCP_OPT_ROUTER:
            out->router_count = take_ip_list(val, vlen, out->router, (uint8_t)DHCP_MAX_ROUTERS);
            break;
        case DHCP_OPT_DNS:
            out->dns_count = take_ip_list(val, vlen, out->dns, (uint8_t)DHCP_MAX_DNS);
            break;
        case DHCP_OPT_SERVER_ID:
            if (vlen == 4u) {
                out->server_id = rd_u32(val);
                out->has_server_id = 1u;
            }
            break;
        case DHCP_OPT_LEASE_TIME:
            if (vlen == 4u) {
                out->lease_time_s = rd_u32(val);
                out->has_lease_time = 1u;
            }
            break;
        case DHCP_OPT_T1:
            if (vlen == 4u) {
                out->t1_s = rd_u32(val);
                out->has_t1 = 1u;
            }
            break;
        case DHCP_OPT_T2:
            if (vlen == 4u) {
                out->t2_s = rd_u32(val);
                out->has_t2 = 1u;
            }
            break;
        case DHCP_OPT_REQUESTED_IP:
            if (vlen == 4u) {
                out->requested_ip = rd_u32(val);
                out->has_requested_ip = 1u;
            }
            break;
        case DHCP_OPT_MAX_MSG_SIZE:
            if (vlen == 2u) {
                out->max_msg_size = rd_u16(val);
                out->has_max_msg_size = 1u;
            }
            break;
        case DHCP_OPT_BROADCAST_ADDR:
            if (vlen == 4u) {
                out->broadcast_addr = rd_u32(val);
                out->has_broadcast_addr = 1u;
            }
            break;
        default:
            /* Unknown option: skip it. */
            break;
        }

        i = (uint16_t)(i + vlen);
    }

    if (!out->has_msg_type) {
        return DHCP_PARSE_ERR_NO_TYPE;
    }
    return DHCP_PARSE_OK;
}

/* ------------------------------------------------------------------------- */
/* Names                                                                     */
/* ------------------------------------------------------------------------- */

const char *dhcp_msg_type_name(uint8_t type)
{
    switch (type) {
    case DHCP_TYPE_DISCOVER: return "DHCPDISCOVER";
    case DHCP_TYPE_OFFER: return "DHCPOFFER";
    case DHCP_TYPE_REQUEST: return "DHCPREQUEST";
    case DHCP_TYPE_DECLINE: return "DHCPDECLINE";
    case DHCP_TYPE_ACK: return "DHCPACK";
    case DHCP_TYPE_NAK: return "DHCPNAK";
    case DHCP_TYPE_RELEASE: return "DHCPRELEASE";
    case DHCP_TYPE_INFORM: return "DHCPINFORM";
    default: return "unknown";
    }
}
