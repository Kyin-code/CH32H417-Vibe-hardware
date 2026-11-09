/*
 * mdns.c -- minimal multicast DNS responder (see mdns.h).
 *
 * Answers A / PTR / SRV / TXT for "<host>.local" and advertises an HTTP service,
 * which is enough for http://ch32h417.local/ on Windows, macOS, Linux (avahi)
 * and phones.
 */
#include "mdns.h"

#include <string.h>

#include "wchnet.h"
#include "eth_driver.h" /* SocketInf[] */

/* ------------------------------------------------------------------------- */
/* Writers                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint8_t *buf;
    uint16_t cap;
    uint16_t pos;
    uint8_t ok;
} wr_t;

static void w8(wr_t *w, uint8_t v)
{
    if (w->ok == 0u || w->pos >= w->cap) {
        w->ok = 0u;
        return;
    }
    w->buf[w->pos++] = v;
}

static void w16(wr_t *w, uint16_t v)
{
    w8(w, (uint8_t)(v >> 8));
    w8(w, (uint8_t)v);
}

static void w32(wr_t *w, uint32_t v)
{
    w8(w, (uint8_t)(v >> 24));
    w8(w, (uint8_t)(v >> 16));
    w8(w, (uint8_t)(v >> 8));
    w8(w, (uint8_t)v);
}

/* DNS name: length-prefixed labels, zero-terminated. */
static void w_name(wr_t *w, const char *name)
{
    const char *p = name;

    while (*p != '\0' && w->ok != 0u) {
        const char *dot = p;
        uint8_t n = 0u;

        while (*dot != '\0' && *dot != '.') {
            dot++;
            n++;
        }
        if (n == 0u || n > 63u) {
            w->ok = 0u;
            return;
        }
        w8(w, n);
        while (p < dot) {
            w8(w, (uint8_t)*p);
            p++;
        }
        if (*p == '.') {
            p++;
        }
    }
    w8(w, 0u);
}

/* Patch a 16-bit length field after writing the payload. */
static void patch_len(wr_t *w, uint16_t len_pos, uint16_t start)
{
    uint16_t n = (uint16_t)(w->pos - start);

    if (w->ok != 0u && (uint32_t)len_pos + 1u < w->cap) {
        w->buf[len_pos] = (uint8_t)(n >> 8);
        w->buf[len_pos + 1u] = (uint8_t)n;
    }
}

/* ------------------------------------------------------------------------- */
/* Readers                                                                   */
/* ------------------------------------------------------------------------- */

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* Read a (possibly compressed) name into `out` (lower-cased, dot form).
 * Returns the offset just past the name, or 0 on a malformed name. */
static uint16_t rd_name(const uint8_t *msg, uint16_t len, uint16_t off, char *out,
                        uint16_t out_cap)
{
    uint16_t p = off;
    uint16_t out_pos = 0u;
    uint16_t after = 0u;
    uint8_t jumped = 0u;
    uint8_t guard = 0u;

    while (p < len && guard < 128u) {
        uint8_t l = msg[p];

        guard++;
        if (l == 0u) {
            p++;
            if (jumped == 0u) {
                after = p;
            }
            break;
        }
        if ((l & 0xC0u) == 0xC0u) {
            uint16_t ptr;

            if ((uint32_t)p + 1u >= len) {
                return 0u;
            }
            ptr = (uint16_t)(((uint16_t)(l & 0x3Fu) << 8) | msg[p + 1u]);
            if (jumped == 0u) {
                after = (uint16_t)(p + 2u);
                jumped = 1u;
            }
            if (ptr >= len) {
                return 0u;
            }
            p = ptr;
            continue;
        }
        if ((uint32_t)p + 1u + l > len) {
            return 0u;
        }
        if (out_pos != 0u && out_pos + 1u < out_cap) {
            out[out_pos++] = '.';
        }
        {
            uint8_t i;
            for (i = 0; i < l; i++) {
                char c = (char)msg[p + 1u + i];
                if (c >= 'A' && c <= 'Z') {
                    c = (char)(c - 'A' + 'a');
                }
                if (out_pos + 1u < out_cap) {
                    out[out_pos++] = c;
                }
            }
        }
        p = (uint16_t)(p + 1u + l);
    }
    if (out_cap > 0u) {
        out[out_pos < out_cap ? out_pos : out_cap - 1u] = '\0';
    }
    return after;
}

static uint8_t name_eq(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0u;
        }
    }
    return (*a == '\0' && *b == '\0') ? 1u : 0u;
}

/* ------------------------------------------------------------------------- */
/* Answer construction                                                       */
/* ------------------------------------------------------------------------- */

#define MDNS_T_A 1u
#define MDNS_T_PTR 12u
#define MDNS_T_TXT 16u
#define MDNS_T_SRV 33u
#define MDNS_CLASS_IN 1u
#define MDNS_CLASS_FLUSH 0x8000u

static const char k_host[] = MDNS_HOST_LABEL ".local";
static const char k_service[] = "_http._tcp.local";
static const char k_instance[] = MDNS_INSTANCE "._http._tcp.local";

static uint16_t build_answer(uint8_t *buf, uint16_t cap, uint32_t ip, uint8_t want_a,
                             uint8_t want_ptr, uint8_t want_srv, uint8_t want_txt)
{
    wr_t w;
    uint16_t answers = 0u;
    uint16_t ancount_pos;

    w.buf = buf;
    w.cap = cap;
    w.pos = 0u;
    w.ok = 1u;

    /* id 0, flags 0x8400 (response + authoritative) */
    w16(&w, 0u);
    w16(&w, 0x8400u);
    w16(&w, 0u); /* qdcount */
    ancount_pos = w.pos;
    w16(&w, 0u); /* ancount, patched below */
    w16(&w, 0u); /* nscount */
    w16(&w, 0u); /* arcount */

    if (want_a != 0u) {
        w_name(&w, k_host);
        w16(&w, MDNS_T_A);
        w16(&w, (uint16_t)(MDNS_CLASS_IN | MDNS_CLASS_FLUSH));
        w32(&w, 120u);
        w16(&w, 4u);
        w32(&w, ip);
        answers++;
    }
    if (want_ptr != 0u) {
        uint16_t len_pos;
        uint16_t start;

        w_name(&w, k_service);
        w16(&w, MDNS_T_PTR);
        w16(&w, MDNS_CLASS_IN);
        w32(&w, 4500u);
        len_pos = w.pos;
        w16(&w, 0u);
        start = w.pos;
        w_name(&w, k_instance);
        patch_len(&w, len_pos, start);
        answers++;
    }
    if (want_srv != 0u) {
        uint16_t len_pos;
        uint16_t start;

        w_name(&w, k_instance);
        w16(&w, MDNS_T_SRV);
        w16(&w, (uint16_t)(MDNS_CLASS_IN | MDNS_CLASS_FLUSH));
        w32(&w, 120u);
        len_pos = w.pos;
        w16(&w, 0u);
        start = w.pos;
        w16(&w, 0u);  /* priority */
        w16(&w, 0u);  /* weight   */
        w16(&w, 80u); /* port     */
        w_name(&w, k_host);
        patch_len(&w, len_pos, start);
        answers++;
    }
    if (want_txt != 0u) {
        static const char txt[] = "path=/";
        uint16_t len_pos;
        uint16_t start;
        uint16_t i;

        w_name(&w, k_instance);
        w16(&w, MDNS_T_TXT);
        w16(&w, (uint16_t)(MDNS_CLASS_IN | MDNS_CLASS_FLUSH));
        w32(&w, 4500u);
        len_pos = w.pos;
        w16(&w, 0u);
        start = w.pos;
        w8(&w, (uint8_t)(sizeof(txt) - 1u));
        for (i = 0; i < (uint16_t)(sizeof(txt) - 1u); i++) {
            w8(&w, (uint8_t)txt[i]);
        }
        patch_len(&w, len_pos, start);
        answers++;
    }

    if (w.ok == 0u) {
        return 0u;
    }
    buf[ancount_pos] = (uint8_t)(answers >> 8);
    buf[ancount_pos + 1u] = (uint8_t)answers;
    return w.pos;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

int mdns_start(mdns_t *m)
{
    SOCK_INF inf;
    uint8_t rc;

    if (m == 0) {
        return -1;
    }
    memset(m, 0, sizeof(*m));

    memset(&inf, 0, sizeof(inf));
    inf.ProtoType = PROTO_TYPE_UDP;
    inf.SourPort = (uint32_t)MDNS_PORT;
    inf.DesPort = (uint32_t)MDNS_PORT;
    /* IPAddr stays 0.0.0.0 so the PCB is bound but not connected; the multicast
     * destination is supplied per send with WCHNET_SocketUdpSendTo(). */
    inf.RecvStartPoint = (uint32_t)(uintptr_t)(void *)m->rx_buf;
    inf.RecvBufLen = (uint32_t)sizeof(m->rx_buf);

    rc = WCHNET_SocketCreat(&m->socket_id, &inf);
    if (rc != WCHNET_ERR_SUCCESS) {
        m->last_rc = rc;
        return -1;
    }
    m->socket_open = 1u;
    WCHNET_ModifyRecvBuf(m->socket_id, inf.RecvStartPoint, inf.RecvBufLen);
    return 0;
}

void mdns_set_address(mdns_t *m, uint32_t ip)
{
    uint8_t buf[192];
    uint8_t group[4];
    uint16_t n;
    uint32_t slen;

    if (m == 0 || m->socket_open == 0u) {
        return;
    }
    if (m->ip == ip && m->announced != 0u) {
        return;
    }
    m->ip = ip;

    n = build_answer(buf, (uint16_t)sizeof(buf), ip, 1u, 1u, 1u, 1u);
    if (n == 0u) {
        return;
    }
    group[0] = MDNS_GROUP_0;
    group[1] = MDNS_GROUP_1;
    group[2] = MDNS_GROUP_2;
    group[3] = MDNS_GROUP_3;
    slen = n;
    (void)WCHNET_SocketUdpSendTo(m->socket_id, buf, &slen, group, MDNS_PORT);
    m->announced = 1u;
}

void mdns_poll(mdns_t *m)
{
    if (m == 0 || m->socket_open == 0u) {
        return;
    }
    (void)WCHNET_GetSocketInt(m->socket_id);

    for (;;) {
        uint32_t len;
        uint8_t buf[192];
        uint8_t group[4];
        uint16_t qd;
        uint16_t i;
        uint16_t off;
        uint8_t want_a = 0u;
        uint8_t want_ptr = 0u;
        uint8_t want_srv = 0u;
        uint8_t want_txt = 0u;
        uint32_t slen;
        uint16_t n;

        len = WCHNET_SocketRecvLen(m->socket_id, 0);
        if (len == 0u) {
            return;
        }
        if (len > sizeof(m->rx_buf)) {
            uint32_t chunk = (uint32_t)sizeof(m->rx_buf);

            (void)WCHNET_SocketRecv(m->socket_id, m->rx_buf, &chunk);
            continue;
        }
        if (WCHNET_SocketRecv(m->socket_id, m->rx_buf, &len) != WCHNET_ERR_SUCCESS) {
            return;
        }
        if (len < 12u) {
            continue;
        }
        if ((m->rx_buf[2] & 0x80u) != 0u) {
            continue; /* a response, not a query */
        }
        qd = rd16(&m->rx_buf[4]);
        off = 12u;
        for (i = 0; i < qd && off < len; i++) {
            char name[96];
            uint16_t qtype;
            uint16_t nxt = rd_name(m->rx_buf, (uint16_t)len, off, name, sizeof(name));

            if (nxt == 0u || (uint32_t)nxt + 4u > len) {
                break;
            }
            qtype = rd16(&m->rx_buf[nxt]);
            off = (uint16_t)(nxt + 4u);

            if (name_eq(name, k_host) != 0 && qtype == MDNS_T_A) {
                want_a = 1u;
            } else if (name_eq(name, k_service) != 0) {
                want_ptr = 1u;
            } else if (name_eq(name, k_instance) != 0) {
                if (qtype == MDNS_T_SRV) {
                    want_srv = 1u;
                } else if (qtype == MDNS_T_TXT) {
                    want_txt = 1u;
                }
            }
        }
        if ((want_a | want_ptr | want_srv | want_txt) == 0u || m->ip == 0u) {
            continue;
        }
        n = build_answer(buf, (uint16_t)sizeof(buf), m->ip, want_a, want_ptr, want_srv, want_txt);
        if (n == 0u) {
            continue;
        }
        group[0] = MDNS_GROUP_0;
        group[1] = MDNS_GROUP_1;
        group[2] = MDNS_GROUP_2;
        group[3] = MDNS_GROUP_3;
        slen = n;
        (void)WCHNET_SocketUdpSendTo(m->socket_id, buf, &slen, group, MDNS_PORT);
    }
}

void mdns_stop(mdns_t *m)
{
    if (m == 0 || m->socket_open == 0u) {
        return;
    }
    (void)WCHNET_SocketClose(m->socket_id, TCP_CLOSE_NORMAL);
    m->socket_open = 0u;
}
