#include "test_support.h"

int g_checks = 0;
int g_failures = 0;

static ts_events_t g_events;

/* ---------------------------------------------------------------------- */

void ts_mac(uint8_t mac[6])
{
    mac[0] = 0x02;
    mac[1] = 0x00;
    mac[2] = 0x00;
    mac[3] = 0x12;
    mac[4] = 0x34;
    mac[5] = 0x56;
}

void ts_default_cfg(dhcp_client_cfg_t *cfg)
{
    dhcp_client_cfg_default(cfg);
    ts_mac(cfg->mac);
    strcpy(cfg->hostname, "kdock-test");
    cfg->retry_base_ms = 1000u;
    cfg->retry_max_ms = 8000u;
    cfg->offer_collect_ms = 100u;
    cfg->decline_wait_ms = 2000u;
    cfg->nak_wait_ms = 2000u;
    cfg->max_retries = 3u;
}

static void ts_on_event(void *user, dhcp_event_t ev, dhcp_state_t old_state,
                        const dhcp_client_t *cl)
{
    (void)user;
    if (g_events.count < TS_MAX_EVENTS) {
        g_events.events[g_events.count] = ev;
        g_events.old_states[g_events.count] = old_state;
        g_events.states[g_events.count] = dhcp_client_get_state(cl);
        g_events.count++;
    }
}

void ts_setup(dhcp_client_t *cl, host_port_t *hp, uint32_t seed)
{
    dhcp_client_cfg_t cfg;

    ts_default_cfg(&cfg);
    ts_setup_with(cl, hp, seed, &cfg);
}

void ts_setup_with(dhcp_client_t *cl, host_port_t *hp, uint32_t seed,
                   const dhcp_client_cfg_t *cfg)
{
    ts_events_reset();
    host_port_init(hp, seed);
    CHECK(dhcp_client_init(cl, cfg, &hp->port, ts_on_event, 0) == 0);
}

ts_events_t *ts_events(void)
{
    return &g_events;
}

void ts_events_reset(void)
{
    memset(&g_events, 0, sizeof(g_events));
}

unsigned ts_event_count(dhcp_event_t ev)
{
    unsigned i;
    unsigned n = 0u;

    for (i = 0; i < g_events.count; i++) {
        if (g_events.events[i] == ev) {
            n++;
        }
    }
    return n;
}

int ts_event_seen(dhcp_event_t ev)
{
    return ts_event_count(ev) > 0u;
}

unsigned ts_state_change_count(void)
{
    return ts_event_count(DHCP_EVENT_STATE_CHANGE);
}

unsigned ts_state_change_inconsistent(void)
{
    unsigned i;
    unsigned bad = 0u;

    for (i = 0; i < g_events.count; i++) {
        if (g_events.events[i] != DHCP_EVENT_STATE_CHANGE) {
            continue;
        }
        if (g_events.old_states[i] == g_events.states[i]) {
            bad++;
        }
    }
    return bad;
}

int ts_state_sequence_matches(const dhcp_state_t *expected, unsigned n)
{
    unsigned i;
    unsigned k = 0u;

    for (i = 0; i < g_events.count; i++) {
        if (g_events.events[i] != DHCP_EVENT_STATE_CHANGE) {
            continue;
        }
        if (k >= n || g_events.states[i] != expected[k]) {
            return 0;
        }
        k++;
    }
    return k == n;
}

int ts_deliver(dhcp_client_t *cl, const uint8_t *buf, uint16_t len)
{
    return dhcp_client_input(cl, buf, len);
}

int ts_deliver_reply(dhcp_client_t *cl, const reply_spec_t *s)
{
    uint8_t buf[600];
    uint16_t n = ts_build_reply(buf, (uint16_t)sizeof(buf), s);

    if (n == 0u) {
        return -99;
    }
    return dhcp_client_input(cl, buf, n);
}

/* ---------------------------------------------------------------------- */

void ts_reply_default(reply_spec_t *s, uint8_t type, uint32_t xid, const uint8_t mac[6],
                      uint32_t server_id, uint32_t addr, uint32_t lease_time)
{
    memset(s, 0, sizeof(*s));
    s->msg_type = type;
    s->xid = xid;
    s->mac = mac;
    s->include_server = 1u;
    s->server_id = server_id;
    s->include_mask = 1u;
    s->mask = 0xFFFFFF00u;
    s->include_router = 1u;
    s->router = 0xC0A80101u; /* 192.168.1.1 */
    s->include_dns2 = 1u;
    s->dns1 = 0x08080808u; /* 8.8.8.8   */
    s->dns2 = 0x08080404u; /* 8.8.4.4   */
    s->include_lease = 1u;
    s->lease_time = lease_time;
    s->yiaddr = addr;
}

uint16_t ts_build_reply(uint8_t *buf, uint16_t cap, const reply_spec_t *s)
{
    uint16_t p = 0u;
    uint8_t mac[6];
    uint8_t i;
    uint8_t hlen = s->hlen != 0u ? s->hlen : 6u;

    if (buf == 0 || cap < 300u) {
        return 0u;
    }
    memset(buf, 0, cap);
    memset(mac, 0, sizeof(mac));
    if (s->mac != 0) {
        memcpy(mac, s->mac, 6u);
    }

#define PUT8(v)                                                                  \
    do {                                                                         \
        if (p >= cap) {                                                          \
            return 0u;                                                           \
        }                                                                        \
        buf[p++] = (uint8_t)(v);                                                 \
    } while (0)
#define PUT16(v)                                                                 \
    do {                                                                         \
        PUT8((uint16_t)(v) >> 8);                                                \
        PUT8((uint16_t)(v) & 0xFFu);                                             \
    } while (0)
#define PUT32(v)                                                                 \
    do {                                                                         \
        PUT8((uint32_t)(v) >> 24);                                               \
        PUT8(((uint32_t)(v) >> 16) & 0xFFu);                                     \
        PUT8(((uint32_t)(v) >> 8) & 0xFFu);                                      \
        PUT8((uint32_t)(v) & 0xFFu);                                             \
    } while (0)

    PUT8(s->op != 0u ? s->op : DHCP_OP_BOOTREPLY);
    PUT8(1u); /* htype */
    PUT8(hlen);
    PUT8(0u); /* hops */
    PUT32(s->xid);
    PUT16(0u); /* secs */
    PUT16(s->flags);
    PUT32(s->ciaddr);
    PUT32(s->yiaddr);
    PUT32(0u); /* siaddr */
    PUT32(0u); /* giaddr */
    for (i = 0; i < 16u; i++) {
        PUT8(i < 6u ? mac[i] : 0u);
    }
    for (i = 0; i < 64u; i++) {
        PUT8(0u);
    }
    for (i = 0; i < 128u; i++) {
        PUT8(0u);
    }
    PUT32(s->no_cookie != 0u ? 0x11223344u : DHCP_MAGIC_COOKIE);

    if (s->no_msg_type == 0u) {
        PUT8(DHCP_OPT_MSG_TYPE);
        PUT8(1u);
        PUT8(s->msg_type);
    }
    if (s->include_server != 0u) {
        PUT8(DHCP_OPT_SERVER_ID);
        PUT8(4u);
        PUT32(s->server_id);
    }
    if (s->include_mask != 0u) {
        PUT8(DHCP_OPT_SUBNET_MASK);
        PUT8(4u);
        PUT32(s->mask);
    }
    if (s->include_router != 0u) {
        PUT8(DHCP_OPT_ROUTER);
        PUT8(4u);
        PUT32(s->router);
    }
    if (s->include_dns2 != 0u) {
        PUT8(DHCP_OPT_DNS);
        PUT8(8u);
        PUT32(s->dns1);
        PUT32(s->dns2);
    }
    if (s->include_lease != 0u) {
        PUT8(DHCP_OPT_LEASE_TIME);
        PUT8(4u);
        PUT32(s->lease_time);
    }
    if (s->include_t1t2 != 0u) {
        PUT8(DHCP_OPT_T1);
        PUT8(4u);
        PUT32(s->t1);
        PUT8(DHCP_OPT_T2);
        PUT8(4u);
        PUT32(s->t2);
    }
    if (s->bad_opt_len != 0u) {
        /* An option whose length field claims more bytes than we provide. */
        PUT8(200u);
        PUT8(s->bad_opt_len);
        PUT8(0u);
    } else {
        PUT8(DHCP_OPT_END);
    }

#undef PUT8
#undef PUT16
#undef PUT32

    if (s->truncate_to != 0u && p > s->truncate_to) {
        p = s->truncate_to;
    }
    return p;
}
