/*
 * dhcp_port_wchnet.c -- CH32H417 / WCHNET transport for the DHCP client.
 *
 * Only the documented WCHNET API is used:
 *   WCHNET_SocketCreat / WCHNET_ModifyRecvBuf / WCHNET_SocketUdpSendTo
 *   WCHNET_SocketSend / WCHNET_SocketRecvLen / WCHNET_SocketRecv
 *   WCHNET_GetSocketInt
 *
 * Notes on the WCH library (established by disassembling libwchnet.a and by
 * reading the EVT examples under doc/
 *
 *  - SOCK_INF.ProtoType selects PROTO_TYPE_UDP or PROTO_TYPE_IP_RAW; in IP_RAW
 *    mode SourPort carries the IP protocol number and the caller supplies only
 *    the L4 payload (see ETH/IPRaw_PING).
 *
 *  - WCHNET_SocketCreat() calls udp_new -> udp_bind -> udp_connect -> udp_recv,
 *    and it calls udp_connect() only when SOCK_INF.IPAddr is non-zero. For a
 *    DHCP client the PCB must stay UNCONNECTED (IPAddr = 0.0.0.0): lwIP only
 *    delivers a datagram to a connected PCB when source IP *and* source port
 *    match, so a PCB connected to 255.255.255.255:67 silently drops the
 *    server's reply from its unicast address. The destination is chosen per
 *    datagram by WCHNET_SocketUdpSendTo() instead.
 *
 *  - The receive buffer is not usable until WCHNET_ModifyRecvBuf() attaches it
 *    to the socket; every vendor example calls it right after SocketCreat().
 *
 *  - Socket data is read by polling WCHNET_SocketRecvLen(); the receive
 *    interrupt bit only signals "data available".
 */
#include "dhcp_port_wchnet.h"

#include <string.h>

#include "wchnet.h"

/* ------------------------------------------------------------------------- */
/* Weak platform hooks                                                       */
/* ------------------------------------------------------------------------- */

#if defined(__GNUC__)
#define DHCP_WEAK __attribute__((weak))
#else
#define DHCP_WEAK
#endif

/* Default: remember the address so the application can pick it up later. */
DHCP_WEAK void dhcp_wchnet_apply_lease(const dhcp_lease_t *lease)
{
    (void)lease;
}

DHCP_WEAK void dhcp_wchnet_lease_save(const uint8_t *blob, uint16_t len)
{
    (void)blob;
    (void)len;
}

DHCP_WEAK int dhcp_wchnet_lease_load(uint8_t *blob, uint16_t cap, uint16_t *len)
{
    (void)blob;
    (void)cap;
    (void)len;
    return -1;
}

DHCP_WEAK void dhcp_wchnet_lease_clear(void)
{
}

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

static uint32_t wchnet_now_ms(void *user)
{
    dhcp_wchnet_ctx_t *ctx = (dhcp_wchnet_ctx_t *)user;
    /* Single aligned 32-bit read of a volatile: atomic on CH32H417. */
    return (uint32_t)ctx->now_ms;
}

static uint32_t wchnet_rand32(void *user)
{
    dhcp_wchnet_ctx_t *ctx = (dhcp_wchnet_ctx_t *)user;
    uint32_t x = ctx->rng;

    /* xorshift32; seeded from the MAC at init. */
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    ctx->rng = x;
    return x;
}

static void wchnet_open_socket(dhcp_wchnet_ctx_t *ctx, const uint8_t dst[4])
{
    SOCK_INF inf;
    uint8_t rc;
    /* A DHCP socket must be BOUND but NOT CONNECTED (see the file header). */
#if DHCP_WCHNET_USE_UDP_SOCKET
    static const uint8_t any[4] = { 0u, 0u, 0u, 0u };
#endif

    if (ctx->socket_open != 0u) {
        (void)WCHNET_SocketClose(ctx->socket_id, TCP_CLOSE_NORMAL);
        ctx->socket_open = 0u;
    }

    memset(&inf, 0, sizeof(inf));
#if DHCP_WCHNET_USE_UDP_SOCKET
    inf.ProtoType = PROTO_TYPE_UDP;
    inf.SourPort = (uint32_t)DHCP_CLIENT_PORT;
    inf.DesPort = (uint32_t)DHCP_SERVER_PORT;
    /* Keep the PCB unconnected: an IPAddr other than 0.0.0.0 makes
     * WCHNET_SocketCreat() call udp_connect(), which filters out the server's
     * replies. */
    memcpy(inf.IPAddr, any, 4u);
#else
    inf.ProtoType = PROTO_TYPE_IP_RAW;
    inf.SourPort = 17u; /* UDP protocol number in IP_RAW mode */
    inf.DesPort = (uint32_t)DHCP_SERVER_PORT;
    /* IP_RAW is intrinsically connection-oriented (raw_connect), so the
     * destination has to be set when the socket is created. */
    memcpy(inf.IPAddr, dst, 4u);
#endif
    memcpy(ctx->dst_ip, dst, 4u);
    inf.RecvStartPoint = (uint32_t)(uintptr_t)(void *)ctx->rx_buf;
    inf.RecvBufLen = (uint32_t)DHCP_WCHNET_RX_BUF_LEN;

    rc = WCHNET_SocketCreat(&ctx->socket_id, &inf);
    if (rc == WCHNET_ERR_SUCCESS) {
        ctx->socket_open = 1u;
        /* WCHNET_SocketCreat() only records the initial SOCK_INF fields; the
         * receive buffer is attached by WCHNET_ModifyRecvBuf(). Without this
         * the socket transmits but never receives. */
        WCHNET_ModifyRecvBuf(ctx->socket_id, inf.RecvStartPoint, inf.RecvBufLen);
    }
}

#if !DHCP_WCHNET_USE_UDP_SOCKET
static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
#endif

static int wchnet_tx_udp(void *user, const uint8_t *data, uint16_t len, uint32_t dst_ip,
                         uint16_t dst_port)
{
    dhcp_wchnet_ctx_t *ctx = (dhcp_wchnet_ctx_t *)user;
    uint8_t dst[4];

    (void)dst_port;
    if (ctx->socket_open == 0u || len == 0u) {
        return -1;
    }
    dhcp_ip4_to_bytes(dst_ip, dst);

#if DHCP_WCHNET_USE_UDP_SOCKET
    {
        /* WCHNET_SocketUdpSendTo() takes a non-const buffer although it only
         * reads it. A union keeps the const-correctness of this port intact
         * without a -Wcast-qual violation. */
        union {
            const uint8_t *cp;
            uint8_t *p;
        } u;
        uint32_t slen = (uint32_t)len;
        uint8_t rc;

        u.cp = data;
        rc = WCHNET_SocketUdpSendTo(ctx->socket_id, u.p, &slen, dst, DHCP_SERVER_PORT);
        if (rc != WCHNET_ERR_SUCCESS) {
            return -1;
        }
        /* slen is in/out: a short write means the driver took only part of the
         * datagram. Report failure so the client retransmits a whole message
         * instead of letting the server see a truncated one. */
        if (slen != (uint32_t)len) {
            return -1;
        }
    }
#else
    {
        /* IP_RAW: the destination is fixed when the socket is created, so a
         * different target (unicast renew after a broadcast discover) means
         * rebuilding the socket. */
        uint8_t frame[DHCP_MAX_PAYLOAD_BYTES + 8u];
        uint32_t total;
        uint8_t rc;

        if (memcmp(dst, ctx->dst_ip, 4u) != 0) {
            wchnet_open_socket(ctx, dst);
            if (ctx->socket_open == 0u) {
                return -1;
            }
        }
        if ((uint32_t)len + 8u > sizeof(frame)) {
            return -1;
        }
        put_u16(&frame[0], (uint16_t)DHCP_CLIENT_PORT); /* source port */
        put_u16(&frame[2], (uint16_t)DHCP_SERVER_PORT); /* dest port   */
        put_u16(&frame[4], (uint16_t)(len + 8u));       /* UDP length  */
        /* Checksum 0 = "not computed", legal for IPv4 and mandatory here
         * because we do not know our source address yet. */
        put_u16(&frame[6], 0u);
        memcpy(&frame[8], data, len);
        total = (uint32_t)len + 8u;
        rc = WCHNET_SocketSend(ctx->socket_id, frame, &total);
        if (rc != WCHNET_ERR_SUCCESS) {
            return -1;
        }
        /* Same in/out semantics: a partial send is a failure. */
        if (total != (uint32_t)len + 8u) {
            return -1;
        }
    }
#endif
    return 0;
}

static void wchnet_log(void *user, dhcp_log_level_t level, const char *msg)
{
    (void)user;
    (void)level;
    (void)msg;
}

static int wchnet_lease_save(void *user, const uint8_t *blob, uint16_t len)
{
    (void)user;
    dhcp_wchnet_lease_save(blob, len);
    return 0;
}

static int wchnet_lease_load(void *user, uint8_t *blob, uint16_t cap, uint16_t *len)
{
    (void)user;
    return dhcp_wchnet_lease_load(blob, cap, len);
}

static int wchnet_lease_clear(void *user)
{
    (void)user;
    dhcp_wchnet_lease_clear();
    return 0;
}

/* Bridge: apply the lease and forward events to the application. */
static void wchnet_event(void *user, dhcp_event_t ev, dhcp_state_t old_state,
                         const dhcp_client_t *cl)
{
    dhcp_wchnet_ctx_t *ctx = (dhcp_wchnet_ctx_t *)user;

    if ((ev == DHCP_EVENT_BOUND || ev == DHCP_EVENT_RENEWED) && cl->lease.valid != 0u) {
        dhcp_wchnet_apply_lease(&cl->lease);
    }
    if (ctx->user_cb != 0) {
        ctx->user_cb(ctx->user, ev, old_state, cl);
    }
}

/* Strip a leading UDP header if the stack handed us one (IP_RAW mode, or a
 * UDP socket implementation that keeps it). Returns the payload offset. */
static uint16_t l4_offset(const uint8_t *buf, uint32_t len)
{
    if (len >= 8u && buf[0] == 0u && buf[1] == (uint8_t)DHCP_SERVER_PORT && buf[2] == 0u &&
        buf[3] == (uint8_t)DHCP_CLIENT_PORT) {
        uint16_t ulen = (uint16_t)(((uint16_t)buf[4] << 8) | buf[5]);
        if ((uint32_t)ulen + 8u == len || (uint32_t)ulen == len) {
            return 8u;
        }
    }
    return 0u;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

int dhcp_wchnet_init(dhcp_wchnet_ctx_t *ctx, dhcp_client_t *cl, const uint8_t mac[6],
                     const char *hostname)
{
    dhcp_client_cfg_t cfg;
    uint8_t i;

    if (ctx == 0 || cl == 0 || mac == 0) {
        return -1;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->client = cl;
    ctx->now_ms = 0u;
    ctx->rng = 0x9E3779B9u ^ ((uint32_t)mac[2] << 16) ^ ((uint32_t)mac[5] << 8) ^ 0x5Au;

    ctx->port.ctx = ctx;
    ctx->port.now_ms = wchnet_now_ms;
    ctx->port.rand32 = wchnet_rand32;
    ctx->port.tx_udp = wchnet_tx_udp;
    ctx->port.log = wchnet_log;
    ctx->port.lease_save = wchnet_lease_save;
    ctx->port.lease_load = wchnet_lease_load;
    ctx->port.lease_clear = wchnet_lease_clear;

    dhcp_client_cfg_default(&cfg);
    for (i = 0; i < 6u; i++) {
        cfg.mac[i] = mac[i];
    }
    if (hostname != 0) {
        uint8_t n = 0u;
        while (hostname[n] != '\0' && n < (uint8_t)(DHCP_HOSTNAME_MAX - 1)) {
            cfg.hostname[n] = hostname[n];
            n++;
        }
        cfg.hostname[n] = '\0';
    }

    return dhcp_client_init(cl, &cfg, &ctx->port, wchnet_event, ctx);
}

int dhcp_wchnet_start(dhcp_wchnet_ctx_t *ctx)
{
    uint8_t bcast[4];

    if (ctx == 0 || ctx->client == 0) {
        return -1;
    }
    dhcp_ip4_to_bytes(DHCP_IP4_BROADCAST, bcast);
    wchnet_open_socket(ctx, bcast);
    if (ctx->socket_open == 0u) {
        return -1;
    }
    return dhcp_client_start(ctx->client);
}

void dhcp_wchnet_tick(dhcp_wchnet_ctx_t *ctx, uint32_t ms)
{
    if (ctx == 0) {
        return;
    }
    /* Called from the periodic timer ISR (same one that feeds
     * WCHNET_TimeIsr()). Single writer, aligned 32-bit read-modify-write, so no
     * locking is needed; see the race analysis in dhcp_port_wchnet.h. */
    ctx->now_ms = (uint32_t)ctx->now_ms + ms;
}

void dhcp_wchnet_poll(dhcp_wchnet_ctx_t *ctx)
{
    uint32_t len;

    if (ctx == 0 || ctx->socket_open == 0u) {
        return;
    }

    /* Read-and-clear this socket's interrupt bits, and keep the raw value so
     * the application can tell "nothing arrived" from "arrived but dropped".
     * The vendor examples do the same inside WCHNET_HandleGlobalInt(). */
    ctx->last_intstat = WCHNET_GetSocketInt(ctx->socket_id);
    if (ctx->last_intstat != 0u) {
        ctx->rx_int_total++;
    }

    for (;;) {
        len = WCHNET_SocketRecvLen(ctx->socket_id, 0);
        if (len == 0u) {
            break;
        }
        if (len > (uint32_t)DHCP_WCHNET_RX_BUF_LEN) {
            /* A DHCP message can never legitimately exceed our staging buffer.
             * Reading it in two chunks would make the parser see two malformed
             * halves, so drain and drop the whole datagram. */
            uint32_t drop = len;

            ctx->rx_oversize++;
            while (drop > 0u) {
                uint32_t chunk = drop > (uint32_t)DHCP_WCHNET_RX_BUF_LEN
                                     ? (uint32_t)DHCP_WCHNET_RX_BUF_LEN
                                     : drop;
                if (WCHNET_SocketRecv(ctx->socket_id, ctx->rx_buf, &chunk) !=
                    WCHNET_ERR_SUCCESS) {
                    break;
                }
                if (chunk == 0u) {
                    break;
                }
                drop -= chunk;
            }
            continue;
        }
        if (WCHNET_SocketRecv(ctx->socket_id, ctx->rx_buf, &len) != WCHNET_ERR_SUCCESS) {
            break;
        }
        if (len > 0u) {
            uint16_t off = l4_offset(ctx->rx_buf, len);

            ctx->rx_frames++;
            if ((uint32_t)off < len) {
                (void)dhcp_client_input(ctx->client, &ctx->rx_buf[off],
                                        (uint16_t)(len - (uint32_t)off));
            }
        }
    }
}

void dhcp_wchnet_set_event_cb(dhcp_wchnet_ctx_t *ctx, dhcp_event_cb_t cb, void *user)
{
    if (ctx == 0) {
        return;
    }
    ctx->user_cb = cb;
    ctx->user = user;
}

int dhcp_wchnet_owns_socket(const dhcp_wchnet_ctx_t *ctx, uint8_t sockid)
{
    if (ctx == 0) {
        return 0;
    }
    return (ctx->socket_open != 0u && ctx->socket_id == sockid) ? 1 : 0;
}

const dhcp_lease_t *dhcp_wchnet_lease(const dhcp_wchnet_ctx_t *ctx)
{
    if (ctx == 0 || ctx->client == 0) {
        return 0;
    }
    return dhcp_client_get_lease(ctx->client);
}
