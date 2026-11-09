/*
 * dhcp_client.c -- DHCPv4 client state machine, timers and lease management.
 *
 * Non-blocking: dhcp_client_poll() is called from the application loop, all
 * timers use the port's monotonic millisecond counter, so the same code runs
 * on a host with a virtual clock (see tests/) and on an MCU.
 *
 * RFC 2131 states implemented: INIT, SELECTING, REQUESTING, BOUND, RENEWING,
 * REBINDING, RELEASING (+ INIT-REBOOT for lease restoration, + STOPPED).
 */
#include "dhcp_client.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

static void dlog(dhcp_client_t *cl, dhcp_log_level_t level, const char *tag, uint32_t ip)
{
#if DHCP_ENABLE_LOG
    char buf[72];
    uint16_t n = 0u;
    const char *t = tag;

    if (cl->port.log == 0) {
        return;
    }
    while (t != 0 && *t != '\0' && n < 40u) {
        buf[n++] = *t++;
    }
    if (ip != 0u) {
        char ipb[16];
        uint16_t k = 0u;
        dhcp_ip4_to_str(ip, ipb);
        buf[n++] = ' ';
        while (ipb[k] != '\0' && k < 15u && n < 70u) {
            buf[n++] = ipb[k++];
        }
    }
    buf[n] = '\0';
    cl->port.log(cl->port.ctx, level, buf);
#else
    (void)cl;
    (void)level;
    (void)tag;
    (void)ip;
#endif
}

/* Report an event. `old_state` is the state the client was in before the
 * transition for DHCP_EVENT_STATE_CHANGE; for every other event it is the
 * state the event happened in. */
static void emit_ev(dhcp_client_t *cl, dhcp_event_t ev, dhcp_state_t old_state)
{
    if (cl->cb != 0) {
        cl->cb(cl->cb_user, ev, old_state, cl);
    }
}

static void set_state(dhcp_client_t *cl, dhcp_state_t next)
{
    dhcp_state_t old = cl->state;

    if (old == next) {
        return;
    }
    cl->state = next;
    cl->state_ms = cl->port.now_ms(cl->port.ctx);
    dlog(cl, DHCP_LOG_DEBUG, dhcp_state_name(next), 0);
    /* The callback must see old != new and cl->state == new. */
    emit_ev(cl, DHCP_EVENT_STATE_CHANGE, old);
}

/* --- configuration accessors with defaults applied ----------------------- */

static uint32_t cfg_retry_base(const dhcp_client_t *cl)
{
    return cl->cfg.retry_base_ms != 0u ? cl->cfg.retry_base_ms : DHCP_DEFAULT_RETRY_BASE_MS;
}

static uint32_t cfg_retry_max(const dhcp_client_t *cl)
{
    return cl->cfg.retry_max_ms != 0u ? cl->cfg.retry_max_ms : DHCP_DEFAULT_RETRY_MAX_MS;
}

static uint32_t cfg_offer_collect(const dhcp_client_t *cl)
{
    return cl->cfg.offer_collect_ms != 0u ? cl->cfg.offer_collect_ms
                                          : DHCP_DEFAULT_OFFER_COLLECT_MS;
}

static uint32_t cfg_decline_wait(const dhcp_client_t *cl)
{
    return cl->cfg.decline_wait_ms != 0u ? cl->cfg.decline_wait_ms
                                         : DHCP_DEFAULT_DECLINE_WAIT_MS;
}

static uint32_t cfg_nak_wait(const dhcp_client_t *cl)
{
    return cl->cfg.nak_wait_ms != 0u ? cl->cfg.nak_wait_ms : DHCP_DEFAULT_NAK_WAIT_MS;
}

static uint8_t cfg_max_retries(const dhcp_client_t *cl)
{
    return cl->cfg.max_retries != 0u ? cl->cfg.max_retries : DHCP_DEFAULT_MAX_RETRIES;
}

static uint32_t gen_xid(dhcp_client_t *cl)
{
    uint32_t r = cl->port.rand32(cl->port.ctx);

    r ^= cl->port.now_ms(cl->port.ctx) * 2654435761u;
    r ^= ((uint32_t)cl->cfg.mac[2] << 24) | ((uint32_t)cl->cfg.mac[3] << 16) |
         ((uint32_t)cl->cfg.mac[4] << 8) | (uint32_t)cl->cfg.mac[5];
    if (r == 0u) {
        r = 0x6A5C3E1Fu;
    }
    return r;
}

/* ------------------------------------------------------------------------- */
/* Transmit                                                                  */
/* ------------------------------------------------------------------------- */

static int send_msg(dhcp_client_t *cl, uint8_t type, uint32_t dst_ip, uint32_t ciaddr,
                    uint32_t req_ip, uint32_t server_id)
{
    dhcp_build_params_t bp;
    uint16_t len;
    uint32_t elapsed_s;

    memset(&bp, 0, sizeof(bp));
    bp.mac = cl->cfg.mac;
    bp.xid = cl->xid;
    elapsed_s = dhcp_time_diff(cl->port.now_ms(cl->port.ctx), cl->boot_ms) / 1000u;
    bp.secs = (uint16_t)(elapsed_s > 0xFFFFu ? 0xFFFFu : elapsed_s);
    bp.flags = cl->cfg.use_broadcast_flag != 0u ? (uint16_t)DHCP_FLAG_BROADCAST : (uint16_t)0;
    bp.ciaddr = ciaddr;
    bp.requested_ip = req_ip;
    bp.server_id = server_id;
    bp.hostname = cl->cfg.send_hostname != 0u ? cl->cfg.hostname : 0;
    bp.max_msg_size = (uint16_t)DHCP_MAX_MSG_SIZE;

    len = dhcp_packet_build(cl->txbuf, (uint16_t)sizeof(cl->txbuf), type, &bp);
    if (len == 0u) {
        dlog(cl, DHCP_LOG_ERROR, "tx build failed", 0);
        return -1;
    }
    if (cl->port.tx_udp(cl->port.ctx, cl->txbuf, len, dst_ip, (uint16_t)DHCP_SERVER_PORT) != 0) {
        cl->stats.tx_errors++;
        dlog(cl, DHCP_LOG_WARN, "tx udp failed", dst_ip);
        return -1;
    }

    switch (type) {
    case DHCP_TYPE_DISCOVER: cl->stats.tx_discover++; break;
    case DHCP_TYPE_REQUEST:  cl->stats.tx_request++;  break;
    case DHCP_TYPE_DECLINE:  cl->stats.tx_decline++;  break;
    case DHCP_TYPE_RELEASE:  cl->stats.tx_release++;  break;
    default: break;
    }
    dlog(cl, DHCP_LOG_INFO, dhcp_msg_type_name(type), dst_ip);
    return 0;
}

/* Arm the first retransmission timer for a freshly sent request. */
static void arm_first_timeout(dhcp_client_t *cl, uint32_t now)
{
    uint32_t iv = cfg_retry_base(cl);

    cl->retry_interval_ms = iv;
    cl->retries = 0u;
    cl->deadline_ms = dhcp_time_add(now, iv);
}

/*
 * Advance the exponential backoff: base -> 2*base -> ... -> capped, with up to
 * +/- 12.5% jitter so a power failure does not make a whole fleet of nodes
 * retransmit in lock step.
 */
static void schedule_retry(dhcp_client_t *cl, uint32_t now)
{
    uint32_t cap = cfg_retry_max(cl);
    uint32_t iv = cl->retry_interval_ms != 0u ? cl->retry_interval_ms : cfg_retry_base(cl);
    uint32_t jitter = iv / 8u;
    uint32_t r;
    uint32_t next;

    if (jitter == 0u) {
        jitter = 1u;
    }
    r = cl->port.rand32(cl->port.ctx) % (jitter * 2u + 1u);
    cl->deadline_ms = dhcp_time_add(now, (iv - jitter) + r);

    next = iv * 2u;
    if (next < iv || next > cap) {
        next = cap;
    }
    cl->retry_interval_ms = next;
    cl->retries++;
    cl->stats.retransmits++;
}

/* ------------------------------------------------------------------------- */
/* Lease bookkeeping                                                         */
/* ------------------------------------------------------------------------- */

static void fill_from_msg(dhcp_lease_t *l, const dhcp_msg_t *m)
{
    uint8_t i;

    memset(l, 0, sizeof(*l));
    l->address = m->yiaddr;
    l->subnet_mask = m->has_subnet_mask != 0u ? m->subnet_mask : 0u;
    l->router = m->router_count > 0u ? m->router[0] : 0u;
    l->dns_count = m->dns_count;
    for (i = 0; i < m->dns_count && i < (uint8_t)DHCP_MAX_DNS; i++) {
        l->dns[i] = m->dns[i];
    }
    l->server_id = m->has_server_id != 0u ? m->server_id : 0u;
    l->lease_time_s = m->has_lease_time != 0u ? m->lease_time_s : 0u;
    l->t1_s = m->has_t1 != 0u ? m->t1_s : 0u;
    l->t2_s = m->has_t2 != 0u ? m->t2_s : 0u;
    l->infinite = 0u;
    l->valid = 0u;
}

/*
 * Validate server supplied T1/T2/lease values (RFC 2131 section 4.4.5) and
 * turn them into absolute deadlines. Anything inconsistent falls back to the
 * 50% / 87.5% defaults.
 */
static void compute_lease_timers(dhcp_client_t *cl, uint32_t now)
{
    dhcp_lease_t *l = &cl->lease;
    uint64_t lease_ms;
    uint64_t t1_ms;
    uint64_t t2_ms;

    if (l->lease_time_s == DHCP_INFINITE_LEASE_S) {
        l->infinite = 1u;
        cl->lease_ms = 0u;
        cl->t1_ms = 0u;
        cl->t2_ms = 0u;
        cl->expire_ms = 0u;
        return;
    }

    /* The server must send option 51; assume one hour if it did not. */
    if (l->lease_time_s == 0u) {
        l->lease_time_s = 3600u;
    }
    l->infinite = 0u;

    if (l->t1_s == 0u || l->t1_s >= l->lease_time_s) {
        l->t1_s = (uint32_t)(((uint64_t)l->lease_time_s * DHCP_DEFAULT_T1_PERMILLE) / 1000u);
    }
    if (l->t2_s == 0u || l->t2_s >= l->lease_time_s || l->t2_s <= l->t1_s) {
        l->t2_s = (uint32_t)(((uint64_t)l->lease_time_s * DHCP_DEFAULT_T2_PERMILLE) / 1000u);
    }
    if (l->t2_s <= l->t1_s) {
        l->t2_s = l->t1_s + 1u;
    }

    lease_ms = (uint64_t)l->lease_time_s * 1000u;
    t1_ms = (uint64_t)l->t1_s * 1000u;
    t2_ms = (uint64_t)l->t2_s * 1000u;

    /* Keep the deadlines inside the 32-bit millisecond space. */
    if (lease_ms > 0x7FFFFF00u) {
        lease_ms = 0x7FFFFF00u;
    }
    if (t1_ms > lease_ms) {
        t1_ms = lease_ms / 2u;
    }
    if (t2_ms > lease_ms) {
        t2_ms = lease_ms;
    }

    cl->lease_ms = (uint32_t)lease_ms;
    cl->t1_ms = dhcp_time_add(now, (uint32_t)t1_ms);
    cl->t2_ms = dhcp_time_add(now, (uint32_t)t2_ms);
    cl->expire_ms = dhcp_time_add(now, (uint32_t)lease_ms);
}

static void persist_lease(dhcp_client_t *cl)
{
#if DHCP_ENABLE_PERSISTENCE
    uint8_t blob[DHCP_LEASE_BLOB_LEN];
    uint16_t n;

    if (cl->port.lease_save == 0) {
        return;
    }
    n = dhcp_lease_serialize(&cl->lease, blob, (uint16_t)sizeof(blob));
    if (n != 0u) {
        (void)cl->port.lease_save(cl->port.ctx, blob, n);
    }
#else
    (void)cl;
#endif
}

static void forget_lease(dhcp_client_t *cl)
{
    memset(&cl->lease, 0, sizeof(cl->lease));
    memset(&cl->candidate, 0, sizeof(cl->candidate));
    cl->offer_seen = 0u;
    cl->lease_ms = 0u;
    cl->t1_ms = 0u;
    cl->t2_ms = 0u;
    cl->expire_ms = 0u;
#if DHCP_ENABLE_PERSISTENCE
    if (cl->port.lease_clear != 0) {
        (void)cl->port.lease_clear(cl->port.ctx);
    }
#endif
}

/* Queue a restart after a hold-off (NAK / DECLINE). */
static void hold_off_restart(dhcp_client_t *cl, uint32_t now, uint32_t wait_ms)
{
    cl->started = 0u;
    cl->restart_pending = 1u;
    set_state(cl, DHCP_STATE_INIT);
    cl->deadline_ms = dhcp_time_add(now, wait_ms);
}

/* ------------------------------------------------------------------------- */
/* Transitions                                                               */
/* ------------------------------------------------------------------------- */

static void enter_selecting(dhcp_client_t *cl)
{
    uint32_t now = cl->port.now_ms(cl->port.ctx);

    cl->started = 1u;
    cl->restart_pending = 0u;
    cl->boot_ms = now;
    cl->offer_seen = 0u;
    memset(&cl->candidate, 0, sizeof(cl->candidate));
    cl->xid = gen_xid(cl);
    set_state(cl, DHCP_STATE_SELECTING);
    (void)send_msg(cl, DHCP_TYPE_DISCOVER, DHCP_IP4_BROADCAST, 0u, 0u, 0u);
    arm_first_timeout(cl, now);
}

static void enter_requesting(dhcp_client_t *cl)
{
    uint32_t now = cl->port.now_ms(cl->port.ctx);

    if (cl->candidate.address == 0u) {
        /* Nothing usable was offered: keep discovering. */
        cl->offer_seen = 0u;
        (void)send_msg(cl, DHCP_TYPE_DISCOVER, DHCP_IP4_BROADCAST, 0u, 0u, 0u);
        arm_first_timeout(cl, now);
        return;
    }
    set_state(cl, DHCP_STATE_REQUESTING);
    (void)send_msg(cl, DHCP_TYPE_REQUEST, DHCP_IP4_BROADCAST, 0u, cl->candidate.address,
                   cl->candidate.server_id);
    arm_first_timeout(cl, now);
}

static void enter_renewing(dhcp_client_t *cl)
{
    uint32_t now = cl->port.now_ms(cl->port.ctx);

    cl->xid = gen_xid(cl);
    set_state(cl, DHCP_STATE_RENEWING);
    (void)send_msg(cl, DHCP_TYPE_REQUEST, cl->lease.server_id, cl->lease.address, 0u, 0u);
    arm_first_timeout(cl, now);
}

static void enter_rebinding(dhcp_client_t *cl)
{
    uint32_t now = cl->port.now_ms(cl->port.ctx);

    cl->xid = gen_xid(cl);
    set_state(cl, DHCP_STATE_REBINDING);
    (void)send_msg(cl, DHCP_TYPE_REQUEST, DHCP_IP4_BROADCAST, cl->lease.address, 0u, 0u);
    arm_first_timeout(cl, now);
}

static void lease_expired(dhcp_client_t *cl)
{
    dlog(cl, DHCP_LOG_WARN, "lease expired", cl->lease.address);
    forget_lease(cl);
    cl->started = 0u;
    emit_ev(cl, DHCP_EVENT_LEASE_EXPIRED, cl->state);
    set_state(cl, DHCP_STATE_INIT);
    (void)dhcp_client_start(cl);
}

/* ------------------------------------------------------------------------- */
/* Receive path                                                              */
/* ------------------------------------------------------------------------- */

static int handle_offer(dhcp_client_t *cl, const dhcp_msg_t *m, uint32_t now)
{
    if (cl->state != DHCP_STATE_SELECTING) {
        cl->stats.rx_ignored++;
        return 0;
    }
    if (m->has_server_id == 0u || m->server_id == 0u || m->yiaddr == 0u) {
        cl->stats.rx_malformed++;
        return -1;
    }
    cl->stats.rx_offer++;
    dlog(cl, DHCP_LOG_INFO, "offer from", m->server_id);

    if (cl->offer_seen != 0u) {
        /* Keep the first acceptable offer; RFC 2131 lets us take any. */
        return 1;
    }
    fill_from_msg(&cl->candidate, m);
    cl->candidate.valid = 0u;
    cl->offer_seen = 1u;
    cl->offer_deadline_ms = dhcp_time_add(now, cfg_offer_collect(cl));
    dlog(cl, DHCP_LOG_INFO, "offer addr", cl->candidate.address);
    return 1;
}

static void apply_ack(dhcp_client_t *cl, const dhcp_msg_t *m, uint32_t now, int is_renew)
{
    dhcp_lease_t l;

    fill_from_msg(&l, m);
    l.valid = 1u;
    /* REQUESTING/RENEWING require option 54 (checked before we get here), so a
     * missing server id can only happen for INIT-REBOOT and REBINDING, where
     * keeping the known server is the right thing to do. */
    if (l.server_id == 0u) {
        l.server_id = is_renew != 0 ? cl->lease.server_id : cl->candidate.server_id;
    }
    cl->lease = l;
    compute_lease_timers(cl, now);

    dlog(cl, DHCP_LOG_INFO, is_renew != 0 ? "lease renewed" : "lease bound", l.address);

    cl->started = 1u;
    cl->offer_seen = 0u;
    cl->retries = 0u;
    set_state(cl, DHCP_STATE_BOUND);
    emit_ev(cl, is_renew != 0 ? DHCP_EVENT_RENEWED : DHCP_EVENT_BOUND, cl->state);
    persist_lease(cl);
}

static int handle_ack(dhcp_client_t *cl, const dhcp_msg_t *m, uint32_t now)
{
    uint32_t expect = 0u;
    uint32_t must_match = 0u;
    int require_server = 0;
    int is_renew = 0;

    switch (cl->state) {
    case DHCP_STATE_REQUESTING:
        /* RFC 2131 4.3.2: the ACK must come from the server we selected. */
        expect = cl->candidate.address;
        require_server = 1;
        must_match = cl->candidate.server_id;
        break;
    case DHCP_STATE_INIT_REBOOT:
        /* Any server may confirm a restored lease; option 54 is optional. */
        expect = cl->lease.address;
        is_renew = 1;
        break;
    case DHCP_STATE_RENEWING:
        /* Unicast to a known server: the reply must be from that server. */
        expect = cl->lease.address;
        is_renew = 1;
        require_server = 1;
        must_match = cl->lease.server_id;
        break;
    case DHCP_STATE_REBINDING:
        /* RFC 2131 4.3.2: after T2 any server may extend the lease. */
        expect = cl->lease.address;
        is_renew = 1;
        break;
    default:
        cl->stats.rx_ignored++;
        return 0;
    }

    if (m->yiaddr == 0u) {
        cl->stats.rx_malformed++;
        return -1;
    }
    /* Never accept an address we did not ask for. */
    if (expect != 0u && m->yiaddr != expect) {
        cl->stats.rx_ignored++;
        return 0;
    }

    if (require_server != 0) {
        if (m->has_server_id == 0u || m->server_id == 0u) {
            /* No way to authenticate the sender: drop it. */
            cl->stats.rx_ignored++;
            return 0;
        }
        if (must_match != 0u && m->server_id != must_match) {
            cl->stats.rx_ignored++;
            return 0;
        }
    } else if (m->has_server_id != 0u && m->server_id == 0u) {
        /* 0.0.0.0 is never a valid server identifier. */
        cl->stats.rx_ignored++;
        return 0;
    }

    if (m->has_lease_time != 0u && m->lease_time_s == 0u) {
        cl->stats.rx_malformed++;
        return -1;
    }

    cl->stats.rx_ack++;
    apply_ack(cl, m, now, is_renew);
    return 1;
}

static int handle_nak(dhcp_client_t *cl, uint32_t now)
{
    switch (cl->state) {
    case DHCP_STATE_REQUESTING:
    case DHCP_STATE_INIT_REBOOT:
    case DHCP_STATE_RENEWING:
    case DHCP_STATE_REBINDING:
        break;
    default:
        cl->stats.rx_ignored++;
        return 0;
    }
    cl->stats.rx_nak++;
    dlog(cl, DHCP_LOG_WARN, "nak received", cl->lease.address);
    forget_lease(cl);
    emit_ev(cl, DHCP_EVENT_NAK, cl->state);
    hold_off_restart(cl, now, cfg_nak_wait(cl));
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

void dhcp_client_cfg_default(dhcp_client_cfg_t *cfg)
{
    if (cfg == 0) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->retry_base_ms = DHCP_DEFAULT_RETRY_BASE_MS;
    cfg->retry_max_ms = DHCP_DEFAULT_RETRY_MAX_MS;
    cfg->offer_collect_ms = DHCP_DEFAULT_OFFER_COLLECT_MS;
    cfg->decline_wait_ms = DHCP_DEFAULT_DECLINE_WAIT_MS;
    cfg->nak_wait_ms = DHCP_DEFAULT_NAK_WAIT_MS;
    cfg->max_retries = DHCP_DEFAULT_MAX_RETRIES;
    cfg->use_broadcast_flag = 1u;
    cfg->send_hostname = 1u;
}

int dhcp_client_init(dhcp_client_t *cl, const dhcp_client_cfg_t *cfg, const dhcp_port_t *port,
                     dhcp_event_cb_t cb, void *cb_user)
{
    if (cl == 0 || cfg == 0 || port == 0) {
        return -1;
    }
    if (!dhcp_port_is_valid(port)) {
        return -1;
    }
    memset(cl, 0, sizeof(*cl));
    cl->cfg = *cfg;
    cl->port = *port;
    cl->cb = cb;
    cl->cb_user = cb_user;
    cl->state = DHCP_STATE_INIT;
    cl->retry_interval_ms = cfg_retry_base(cl);
    cl->xid = gen_xid(cl);
    return 0;
}

int dhcp_client_start(dhcp_client_t *cl)
{
    if (cl == 0 || !dhcp_port_is_valid(&cl->port)) {
        return -1;
    }
    enter_selecting(cl);
    return 0;
}

int dhcp_client_restore(dhcp_client_t *cl, const dhcp_lease_t *lease)
{
    uint32_t now;

    if (cl == 0 || lease == 0 || !dhcp_port_is_valid(&cl->port)) {
        return -1;
    }
    if (lease->address == 0u) {
        return -1;
    }
    now = cl->port.now_ms(cl->port.ctx);

    cl->lease = *lease;
    cl->lease.valid = 1u;
    cl->candidate = *lease;
    cl->started = 1u;
    cl->restart_pending = 0u;
    cl->offer_seen = 0u;
    cl->boot_ms = now;
    cl->xid = gen_xid(cl);
    compute_lease_timers(cl, now);

    set_state(cl, DHCP_STATE_INIT_REBOOT);
    /* RFC 2131 section 4.3.2: broadcast, option 50 = requested address, no
     * server identifier so any server may confirm or deny. */
    (void)send_msg(cl, DHCP_TYPE_REQUEST, DHCP_IP4_BROADCAST, 0u, cl->lease.address, 0u);
    arm_first_timeout(cl, now);
    return 0;
}

void dhcp_client_poll(dhcp_client_t *cl)
{
    uint32_t now;

    if (cl == 0 || !dhcp_port_is_valid(&cl->port)) {
        return;
    }
    now = cl->port.now_ms(cl->port.ctx);

    switch (cl->state) {
    case DHCP_STATE_INIT:
        if (cl->restart_pending != 0u && dhcp_time_reached(now, cl->deadline_ms)) {
            cl->restart_pending = 0u;
            (void)dhcp_client_start(cl);
        }
        break;

    case DHCP_STATE_SELECTING:
        if (cl->offer_seen != 0u) {
            if (dhcp_time_reached(now, cl->offer_deadline_ms)) {
                enter_requesting(cl);
            }
        } else if (dhcp_time_reached(now, cl->deadline_ms)) {
            if (cl->retries >= cfg_max_retries(cl)) {
                dlog(cl, DHCP_LOG_ERROR, "no offer, giving up", 0);
                cl->started = 0u;
                set_state(cl, DHCP_STATE_INIT);
                emit_ev(cl, DHCP_EVENT_FAILED, cl->state);
            } else {
                (void)send_msg(cl, DHCP_TYPE_DISCOVER, DHCP_IP4_BROADCAST, 0u, 0u, 0u);
                schedule_retry(cl, now);
            }
        }
        break;

    case DHCP_STATE_REQUESTING:
        if (dhcp_time_reached(now, cl->deadline_ms)) {
            if (cl->retries >= cfg_max_retries(cl)) {
                dlog(cl, DHCP_LOG_WARN, "request timed out, rediscovering", 0);
                cl->offer_seen = 0u;
                memset(&cl->candidate, 0, sizeof(cl->candidate));
                enter_selecting(cl);
            } else {
                (void)send_msg(cl, DHCP_TYPE_REQUEST, DHCP_IP4_BROADCAST, 0u,
                               cl->candidate.address, cl->candidate.server_id);
                schedule_retry(cl, now);
            }
        }
        break;

    case DHCP_STATE_INIT_REBOOT:
        if (dhcp_time_reached(now, cl->deadline_ms)) {
            if (cl->retries >= cfg_max_retries(cl)) {
                dlog(cl, DHCP_LOG_WARN, "lease restore failed", cl->lease.address);
                forget_lease(cl);
                enter_selecting(cl);
            } else {
                (void)send_msg(cl, DHCP_TYPE_REQUEST, DHCP_IP4_BROADCAST, 0u, cl->lease.address, 0u);
                schedule_retry(cl, now);
            }
        }
        break;

    case DHCP_STATE_BOUND:
        if (cl->lease.infinite != 0u) {
            break;
        }
        if (dhcp_time_reached(now, cl->expire_ms)) {
            lease_expired(cl);
        } else if (dhcp_time_reached(now, cl->t1_ms)) {
            enter_renewing(cl);
        }
        break;

    case DHCP_STATE_RENEWING:
        if (dhcp_time_reached(now, cl->expire_ms)) {
            lease_expired(cl);
        } else if (dhcp_time_reached(now, cl->t2_ms)) {
            enter_rebinding(cl);
        } else if (dhcp_time_reached(now, cl->deadline_ms)) {
            (void)send_msg(cl, DHCP_TYPE_REQUEST, cl->lease.server_id, cl->lease.address, 0u, 0u);
            schedule_retry(cl, now);
        }
        break;

    case DHCP_STATE_REBINDING:
        if (dhcp_time_reached(now, cl->expire_ms)) {
            lease_expired(cl);
        } else if (dhcp_time_reached(now, cl->deadline_ms)) {
            (void)send_msg(cl, DHCP_TYPE_REQUEST, DHCP_IP4_BROADCAST, cl->lease.address, 0u, 0u);
            schedule_retry(cl, now);
        }
        break;

    case DHCP_STATE_RELEASING:
    case DHCP_STATE_STOPPED:
    default:
        break;
    }
}

int dhcp_client_input(dhcp_client_t *cl, const uint8_t *data, uint16_t len)
{
    dhcp_msg_t msg;
    dhcp_parse_result_t pr;
    uint32_t now;

    if (cl == 0) {
        return -1;
    }
    if (data == 0 || len == 0u) {
        cl->stats.rx_malformed++;
        return -1;
    }

    pr = dhcp_packet_parse(data, len, &msg);
    if (pr != DHCP_PARSE_OK) {
        cl->stats.rx_malformed++;
        dlog(cl, DHCP_LOG_DEBUG, "malformed packet", 0);
        return -1;
    }
    now = cl->port.now_ms(cl->port.ctx);

    if (msg.op != DHCP_OP_BOOTREPLY) {
        cl->stats.rx_ignored++;
        return 0;
    }
#if DHCP_STRICT_REPLY_CHECK
    if (msg.xid != cl->xid) {
        cl->stats.rx_ignored++;
        return 0;
    }
    if (memcmp(msg.chaddr, cl->cfg.mac, 6) != 0) {
        cl->stats.rx_ignored++;
        return 0;
    }
#endif

    switch (msg.msg_type) {
    case DHCP_TYPE_OFFER:
        return handle_offer(cl, &msg, now);
    case DHCP_TYPE_ACK:
        return handle_ack(cl, &msg, now);
    case DHCP_TYPE_NAK:
        return handle_nak(cl, now);
    default:
        cl->stats.rx_ignored++;
        return 0;
    }
}

void dhcp_client_release(dhcp_client_t *cl)
{
    uint32_t now;

    if (cl == 0) {
        return;
    }
    now = cl->port.now_ms(cl->port.ctx);

    if (cl->lease.valid != 0u && cl->lease.address != 0u) {
        uint32_t dst = cl->lease.server_id != 0u ? cl->lease.server_id : DHCP_IP4_BROADCAST;

        set_state(cl, DHCP_STATE_RELEASING);
        (void)send_msg(cl, DHCP_TYPE_RELEASE, dst, cl->lease.address, 0u, cl->lease.server_id);
        emit_ev(cl, DHCP_EVENT_RELEASED, cl->state);
    }
    forget_lease(cl);
    cl->started = 0u;
    cl->restart_pending = 0u;
    cl->deadline_ms = now;
    set_state(cl, DHCP_STATE_STOPPED);
}

void dhcp_client_decline(dhcp_client_t *cl)
{
    uint32_t now;
    uint32_t bad_ip;
    uint32_t server;

    if (cl == 0) {
        return;
    }
    if (cl->state != DHCP_STATE_BOUND && cl->state != DHCP_STATE_REQUESTING) {
        return;
    }
    now = cl->port.now_ms(cl->port.ctx);
    bad_ip = cl->state == DHCP_STATE_BOUND ? cl->lease.address : cl->candidate.address;
    server = cl->state == DHCP_STATE_BOUND ? cl->lease.server_id : cl->candidate.server_id;

    (void)send_msg(cl, DHCP_TYPE_DECLINE, DHCP_IP4_BROADCAST, 0u, bad_ip, server);
    dlog(cl, DHCP_LOG_WARN, "declining address", bad_ip);
    forget_lease(cl);
    emit_ev(cl, DHCP_EVENT_DECLINED, cl->state);
    hold_off_restart(cl, now, cfg_decline_wait(cl));
}

void dhcp_client_stop(dhcp_client_t *cl)
{
    if (cl == 0) {
        return;
    }
    cl->started = 0u;
    cl->restart_pending = 0u;
    set_state(cl, DHCP_STATE_STOPPED);
}

dhcp_state_t dhcp_client_get_state(const dhcp_client_t *cl)
{
    return cl != 0 ? cl->state : DHCP_STATE_INIT;
}

const dhcp_lease_t *dhcp_client_get_lease(const dhcp_client_t *cl)
{
    return cl != 0 ? &cl->lease : 0;
}

const dhcp_stats_t *dhcp_client_get_stats(const dhcp_client_t *cl)
{
    return cl != 0 ? &cl->stats : 0;
}

uint32_t dhcp_client_next_timeout_ms(const dhcp_client_t *cl)
{
    uint32_t now;
    uint32_t best = 0u;
    int have = 0;

    if (cl == 0 || !dhcp_port_is_valid(&cl->port)) {
        return DHCP_POLL_INFINITE;
    }
    now = cl->port.now_ms(cl->port.ctx);

    /* Wrap-safe minimum over all deadlines that are relevant in this state. */
#define DHCP_CONSIDER(d)                            \
    do {                                            \
        uint32_t d_ = (d);                          \
        if (have == 0 || (int32_t)(d_ - best) < 0) { \
            best = d_;                              \
            have = 1;                               \
        }                                           \
    } while (0)

    switch (cl->state) {
    case DHCP_STATE_INIT:
        if (cl->restart_pending != 0u) {
            DHCP_CONSIDER(cl->deadline_ms);
        }
        break;
    case DHCP_STATE_SELECTING:
        DHCP_CONSIDER(cl->offer_seen != 0u ? cl->offer_deadline_ms : cl->deadline_ms);
        break;
    case DHCP_STATE_REQUESTING:
        DHCP_CONSIDER(cl->deadline_ms);
        if (cl->expire_ms != 0u) {
            DHCP_CONSIDER(cl->expire_ms);
        }
        break;
    case DHCP_STATE_INIT_REBOOT:
    case DHCP_STATE_REBINDING:
        DHCP_CONSIDER(cl->deadline_ms);
        if (cl->lease.infinite == 0u && cl->expire_ms != 0u) {
            DHCP_CONSIDER(cl->expire_ms);
        }
        break;
    case DHCP_STATE_RENEWING:
        /* T2 has to wake the scheduler up even when the retransmit timer is
         * longer than the remaining time to T2. */
        DHCP_CONSIDER(cl->deadline_ms);
        if (cl->t2_ms != 0u) {
            DHCP_CONSIDER(cl->t2_ms);
        }
        if (cl->lease.infinite == 0u && cl->expire_ms != 0u) {
            DHCP_CONSIDER(cl->expire_ms);
        }
        break;
    case DHCP_STATE_BOUND:
        if (cl->lease.infinite == 0u) {
            DHCP_CONSIDER(cl->t1_ms);
        }
        break;
    default:
        break;
    }
#undef DHCP_CONSIDER

    if (have == 0) {
        return DHCP_POLL_INFINITE;
    }
    return dhcp_time_reached(now, best) ? DHCP_POLL_NOW : dhcp_time_diff(best, now);
}

const char *dhcp_state_name(dhcp_state_t state)
{
    switch (state) {
    case DHCP_STATE_INIT: return "INIT";
    case DHCP_STATE_SELECTING: return "SELECTING";
    case DHCP_STATE_REQUESTING: return "REQUESTING";
    case DHCP_STATE_BOUND: return "BOUND";
    case DHCP_STATE_RENEWING: return "RENEWING";
    case DHCP_STATE_REBINDING: return "REBINDING";
    case DHCP_STATE_RELEASING: return "RELEASING";
    case DHCP_STATE_INIT_REBOOT: return "INIT-REBOOT";
    case DHCP_STATE_STOPPED: return "STOPPED";
    default: return "?";
    }
}
