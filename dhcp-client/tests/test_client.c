/*
 * test_client.c -- state machine, timers, lease handling.
 *
 * Everything runs on a virtual clock, so "waiting 20 seconds for the lease to
 * expire" costs microseconds.
 */
#include "test_support.h"

#define SERVER_IP 0xC0A80101u /* 192.168.1.1  */
#define LEASED_IP 0xC0A80164u /* 192.168.1.100 */

static const uint32_t k_server = SERVER_IP;
static const uint32_t k_addr = LEASED_IP;

static void run(dhcp_client_t *cl, host_port_t *hp, uint32_t ms, uint32_t step)
{
    host_run_for(cl, hp, ms, step);
}

static int tx_has_req_ip(const host_tx_t *tx)
{
    dhcp_msg_t m;
    if (tx == 0 || dhcp_packet_parse(tx->data, tx->len, &m) != DHCP_PARSE_OK) {
        return 0;
    }
    return m.has_requested_ip != 0u;
}

static int tx_has_server_id(const host_tx_t *tx)
{
    dhcp_msg_t m;
    if (tx == 0 || dhcp_packet_parse(tx->data, tx->len, &m) != DHCP_PARSE_OK) {
        return 0;
    }
    return m.has_server_id != 0u;
}

static uint32_t tx_req_ip(const host_tx_t *tx)
{
    dhcp_msg_t m;
    if (tx == 0 || dhcp_packet_parse(tx->data, tx->len, &m) != DHCP_PARSE_OK) {
        return 0u;
    }
    return m.requested_ip;
}

static uint32_t tx_ciaddr(const host_tx_t *tx)
{
    dhcp_msg_t m;
    if (tx == 0 || dhcp_packet_parse(tx->data, tx->len, &m) != DHCP_PARSE_OK) {
        return 0u;
    }
    return m.ciaddr;
}

/* Drive the client all the way to BOUND with a 60 second lease. */
static void acquire(dhcp_client_t *cl, host_port_t *hp, uint32_t lease_time)
{
    reply_spec_t s;
    uint8_t mac[6];

    ts_mac(mac);
    CHECK(dhcp_client_start(cl) == 0);
    CHECK_U32(dhcp_client_get_state(cl), DHCP_STATE_SELECTING);

    ts_reply_default(&s, TS_OFFER, cl->xid, mac, k_server, k_addr, lease_time);
    CHECK_U32(ts_deliver_reply(cl, &s), 1);
    run(cl, hp, 150u, 10u);
    CHECK_U32(dhcp_client_get_state(cl), DHCP_STATE_REQUESTING);

    ts_reply_default(&s, TS_ACK, cl->xid, mac, k_server, k_addr, lease_time);
    CHECK_U32(ts_deliver_reply(cl, &s), 1);
    CHECK_U32(dhcp_client_get_state(cl), DHCP_STATE_BOUND);
}

/* ------------------------------------------------------------------------- */
/* Happy path                                                                */
/* ------------------------------------------------------------------------- */

static void test_acquire(void)
{
    dhcp_client_t cl;
    host_port_t hp;
    uint8_t mac[6];
    reply_spec_t s;
    const host_tx_t *tx;
    const dhcp_lease_t *lease;

    ts_setup(&cl, &hp, 1u);
    ts_mac(mac);

    CHECK(dhcp_client_start(&cl) == 0);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_SELECTING);
    CHECK_U32(host_count_type(&hp, DHCP_TYPE_DISCOVER), 1u);
    tx = host_last_tx(&hp);
    CHECK(tx != 0);
    if (tx != 0) {
        CHECK_U32(tx->dst_ip, DHCP_IP4_BROADCAST);
        CHECK_U32(tx->dst_port, 67u);
    }

    /* OFFER: the client keeps collecting offers for offer_collect_ms. */
    ts_reply_default(&s, TS_OFFER, cl.xid, mac, k_server, k_addr, 3600u);
    CHECK_U32(ts_deliver_reply(&cl, &s), 1);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_SELECTING);
    CHECK_U32(cl.stats.rx_offer, 1u);

    run(&cl, &hp, 150u, 10u);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_REQUESTING);
    tx = host_last_tx(&hp);
    CHECK_U32(host_tx_msg_type(tx), DHCP_TYPE_REQUEST);
    CHECK_U32(tx_has_req_ip(tx), 1u);
    CHECK_U32(tx_req_ip(tx), k_addr);
    CHECK_U32(tx_has_server_id(tx), 1u);

    /* ACK -> BOUND */
    ts_reply_default(&s, TS_ACK, cl.xid, mac, k_server, k_addr, 3600u);
    CHECK_U32(ts_deliver_reply(&cl, &s), 1u);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_BOUND);

    lease = dhcp_client_get_lease(&cl);
    CHECK_U32(lease->valid, 1u);
    CHECK_U32(lease->address, k_addr);
    CHECK_U32(lease->subnet_mask, 0xFFFFFF00u);
    CHECK_U32(lease->router, k_server);
    CHECK_U32(lease->dns_count, 2u);
    CHECK_U32(lease->dns[0], 0x08080808u);
    CHECK_U32(lease->server_id, k_server);
    CHECK_U32(lease->lease_time_s, 3600u);
    CHECK_U32(lease->t1_s, 1800u); /* 50%   */
    CHECK_U32(lease->t2_s, 3150u); /* 87.5% */
    CHECK_U32(lease->infinite, 0u);

    CHECK_U32(ts_event_seen(DHCP_EVENT_BOUND), 1);
    CHECK_U32(ts_event_seen(DHCP_EVENT_RENEWED), 0);
    /* lease persisted for the next boot */
    CHECK_U32(hp.nvm_written, 1);
}

static void test_acquire_uses_server_t1_t2(void)
{
    dhcp_client_t cl;
    host_port_t hp;
    uint8_t mac[6];
    reply_spec_t s;

    ts_setup(&cl, &hp, 2u);
    ts_mac(mac);
    CHECK(dhcp_client_start(&cl) == 0);

    ts_reply_default(&s, TS_OFFER, cl.xid, mac, k_server, k_addr, 100u);
    CHECK_U32(ts_deliver_reply(&cl, &s), 1);
    run(&cl, &hp, 150u, 10u);

    ts_reply_default(&s, TS_ACK, cl.xid, mac, k_server, k_addr, 100u);
    s.include_t1t2 = 1u;
    s.t1 = 20u;
    s.t2 = 70u;
    CHECK_U32(ts_deliver_reply(&cl, &s), 1);
    CHECK_U32(cl.lease.t1_s, 20u);
    CHECK_U32(cl.lease.t2_s, 70u);
}

static void test_acquire_rejects_bogus_t1_t2(void)
{
    dhcp_client_t cl;
    host_port_t hp;
    uint8_t mac[6];
    reply_spec_t s;

    ts_setup(&cl, &hp, 3u);
    ts_mac(mac);
    CHECK(dhcp_client_start(&cl) == 0);

    ts_reply_default(&s, TS_OFFER, cl.xid, mac, k_server, k_addr, 100u);
    CHECK_U32(ts_deliver_reply(&cl, &s), 1);
    run(&cl, &hp, 150u, 10u);

    /* T1 == T2 == lease: falls back to the 50% / 87.5% defaults. */
    ts_reply_default(&s, TS_ACK, cl.xid, mac, k_server, k_addr, 100u);
    s.include_t1t2 = 1u;
    s.t1 = 100u;
    s.t2 = 100u;
    CHECK_U32(ts_deliver_reply(&cl, &s), 1);
    CHECK_U32(cl.lease.t1_s, 50u);
    CHECK_U32(cl.lease.t2_s, 87u);
}

/* ------------------------------------------------------------------------- */
/* Loss / retry / backoff                                                    */
/* ------------------------------------------------------------------------- */

static void test_retransmit_backoff_then_failed(void)
{
    dhcp_client_t cl;
    host_port_t hp;

    ts_setup(&cl, &hp, 4u);
    CHECK(dhcp_client_start(&cl) == 0);
    CHECK_U32(cl.stats.tx_discover, 1u);

    /* retry_base 1000 ms, doubling: disconnects at 1s, 3s, 7s, 15s.
     * max_retries = 3 => 4 DISCOVERs in total, then FAILED. */
    run(&cl, &hp, 900u, 50u);
    CHECK_U32(cl.stats.tx_discover, 1u);

    run(&cl, &hp, 300u, 50u);
    CHECK_U32(cl.stats.tx_discover, 2u);

    run(&cl, &hp, 5000u, 50u);
    CHECK(cl.stats.tx_discover >= 3u);
    CHECK(cl.stats.retransmits >= 2u);

    run(&cl, &hp, 30000u, 100u);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_INIT);
    CHECK_U32(ts_event_seen(DHCP_EVENT_FAILED), 1);
    CHECK_U32(cl.started, 0u);
}

static void test_offer_after_retransmit_still_works(void)
{
    dhcp_client_t cl;
    host_port_t hp;
    uint8_t mac[6];
    reply_spec_t s;

    ts_setup(&cl, &hp, 5u);
    ts_mac(mac);
    CHECK(dhcp_client_start(&cl) == 0);

    /* lose the first DISCOVER */
    run(&cl, &hp, 1200u, 50u);
    CHECK_U32(cl.stats.tx_discover, 2u);

    ts_reply_default(&s, TS_OFFER, cl.xid, mac, k_server, k_addr, 600u);
    CHECK_U32(ts_deliver_reply(&cl, &s), 1);
    run(&cl, &hp, 150u, 10u);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_REQUESTING);

    ts_reply_default(&s, TS_ACK, cl.xid, mac, k_server, k_addr, 600u);
    CHECK_U32(ts_deliver_reply(&cl, &s), 1);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_BOUND);
}

static void test_request_timeout_restarts_discovery(void)
{
    dhcp_client_t cl;
    host_port_t hp;
    uint8_t mac[6];
    reply_spec_t s;
    unsigned before;

    ts_setup(&cl, &hp, 6u);
    ts_mac(mac);
    CHECK(dhcp_client_start(&cl) == 0);
    ts_reply_default(&s, TS_OFFER, cl.xid, mac, k_server, k_addr, 600u);
    CHECK_U32(ts_deliver_reply(&cl, &s), 1);
    run(&cl, &hp, 150u, 10u);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_REQUESTING);

    before = cl.stats.tx_discover;
    /* no ACK at all: after max_retries the client goes back to DISCOVER */
    run(&cl, &hp, 8000u, 50u);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_SELECTING);
    CHECK(cl.stats.tx_discover > before);
}

/* ------------------------------------------------------------------------- */
/* NAK                                                                       */
/* ------------------------------------------------------------------------- */

static void test_nak_in_requesting(void)
{
    dhcp_client_t cl;
    host_port_t hp;
    uint8_t mac[6];
    reply_spec_t s;
    uint32_t t_before;

    ts_setup(&cl, &hp, 7u);
    ts_mac(mac);
    CHECK(dhcp_client_start(&cl) == 0);
    ts_reply_default(&s, TS_OFFER, cl.xid, mac, k_server, k_addr, 600u);
    CHECK_U32(ts_deliver_reply(&cl, &s), 1);
    run(&cl, &hp, 150u, 10u);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_REQUESTING);

    t_before = hp.now_ms;
    ts_reply_default(&s, TS_NAK, cl.xid, mac, k_server, 0u, 0u);
    s.include_lease = 0u;
    s.include_mask = 0u;
    CHECK_U32(ts_deliver_reply(&cl, &s), 1);

    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_INIT);
    CHECK_U32(ts_event_seen(DHCP_EVENT_NAK), 1);
    CHECK_U32(cl.lease.valid, 0u);
    CHECK_U32(cl.candidate.address, 0u);

    /* hold-off (nak_wait_ms = 2000) then a fresh DISCOVER */
    run(&cl, &hp, 1500u, 50u);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_INIT);
    run(&cl, &hp, 1200u, 50u);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_SELECTING);
    CHECK(hp.now_ms >= t_before + 2000u);
    CHECK_U32(cl.stats.rx_nak, 1u);
}

/* ------------------------------------------------------------------------- */
/* Renew / rebind / expiry                                                   */
/* ------------------------------------------------------------------------- */

static void test_renew_at_t1(void)
{
    dhcp_client_t cl;
    host_port_t hp;
    uint8_t mac[6];
    reply_spec_t s;
    const host_tx_t *tx;

    ts_setup(&cl, &hp, 8u);
    ts_mac(mac);
    acquire(&cl, &hp, 20u); /* t1 = 10s, t2 = 17s, expires at 20s */
    CHECK_U32(ts_event_seen(DHCP_EVENT_BOUND), 1);
    host_clear_tx(&hp);

    run(&cl, &hp, 9500u, 50u);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_BOUND);

    run(&cl, &hp, 800u, 50u);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_RENEWING);
    tx = host_last_tx(&hp);
    CHECK_U32(host_tx_msg_type(tx), DHCP_TYPE_REQUEST);
    if (tx != 0) {
        CHECK_U32(tx->dst_ip, k_server); /* unicast to our server */
        CHECK_U32(tx_ciaddr(tx), k_addr); /* ciaddr filled in */
        CHECK_U32(tx_has_req_ip(tx), 0u); /* no option 50 while renewing */
        CHECK_U32(tx_has_server_id(tx), 0u);
    }

    ts_reply_default(&s, TS_ACK, cl.xid, mac, k_server, k_addr, 20u);
    CHECK_U32(ts_deliver_reply(&cl, &s), 1);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_BOUND);
    CHECK_U32(ts_event_seen(DHCP_EVENT_RENEWED), 1);
}

static void test_renew_timeout_goes_rebinding(void)
{
    dhcp_client_t cl;
    host_port_t hp;
    uint8_t mac[6];
    reply_spec_t s;
    const host_tx_t *tx;

    ts_setup(&cl, &hp, 9u);
    ts_mac(mac);
    acquire(&cl, &hp, 20u);
    host_clear_tx(&hp);

    run(&cl, &hp, 10300u, 50u);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_RENEWING);
    CHECK(cl.stats.tx_request >= 2u); /* retransmitted at least once */

    run(&cl, &hp, 7000u, 50u);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_REBINDING);
    tx = host_last_tx(&hp);
    CHECK_U32(tx->dst_ip, DHCP_IP4_BROADCAST);
    CHECK_U32(tx_ciaddr(tx), k_addr);

    ts_reply_default(&s, TS_ACK, cl.xid, mac, k_server, k_addr, 20u);
    CHECK_U32(ts_deliver_reply(&cl, &s), 1);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_BOUND);
}

static void test_lease_expiry(void)
{
    dhcp_client_t cl;
    host_port_t hp;
    uint8_t mac[6];
    unsigned discover_before;

    ts_setup(&cl, &hp, 10u);
    ts_mac(mac);
    acquire(&cl, &hp, 20u);
    discover_before = cl.stats.tx_discover;

    run(&cl, &hp, 21000u, 100u);
    CHECK_U32(ts_event_seen(DHCP_EVENT_LEASE_EXPIRED), 1);
    CHECK_U32(cl.lease.valid, 0u);
    CHECK_U32(cl.lease.address, 0u);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_SELECTING);
    CHECK(cl.stats.tx_discover > discover_before);
    /* the stale lease must not be resurrected from NVM */
    CHECK_U32(hp.nvm_written, 0);
}

static void test_infinite_lease_never_renews(void)
{
    dhcp_client_t cl;
    host_port_t hp;
    uint8_t mac[6];
    reply_spec_t s;

    ts_setup(&cl, &hp, 11u);
    ts_mac(mac);
    CHECK(dhcp_client_start(&cl) == 0);
    ts_reply_default(&s, TS_OFFER, cl.xid, mac, k_server, k_addr, DHCP_INFINITE_LEASE_S);
    CHECK_U32(ts_deliver_reply(&cl, &s), 1);
    run(&cl, &hp, 150u, 10u);
    ts_reply_default(&s, TS_ACK, cl.xid, mac, k_server, k_addr, DHCP_INFINITE_LEASE_S);
    CHECK_U32(ts_deliver_reply(&cl, &s), 1);

    CHECK_U32(cl.lease.infinite, 1u);
    CHECK_U32(dhcp_client_next_timeout_ms(&cl), DHCP_POLL_INFINITE);
    run(&cl, &hp, 100000u, 1000u);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_BOUND);
}

/* ------------------------------------------------------------------------- */
/* Hostile / unexpected input                                                */
/* ------------------------------------------------------------------------- */

static void test_ignores_foreign_replies(void)
{
    dhcp_client_t cl;
    host_port_t hp;
    uint8_t mac[6];
    uint8_t bad_mac[6];
    reply_spec_t s;

    ts_setup(&cl, &hp, 12u);
    ts_mac(mac);
    CHECK(dhcp_client_start(&cl) == 0);

    /* wrong xid */
    ts_reply_default(&s, TS_OFFER, cl.xid ^ 0xFFu, mac, k_server, k_addr, 600u);
    CHECK_U32(ts_deliver_reply(&cl, &s), 0);
    CHECK_U32(cl.stats.rx_ignored, 1u);

    /* wrong client MAC */
    memcpy(bad_mac, mac, 6u);
    bad_mac[5] ^= 0xFFu;
    ts_reply_default(&s, TS_OFFER, cl.xid, bad_mac, k_server, k_addr, 600u);
    CHECK_U32(ts_deliver_reply(&cl, &s), 0);

    /* a BOOTREQUEST instead of a reply */
    {
        uint8_t req[DHCP_MAX_PAYLOAD_BYTES];
        uint16_t n = dhcp_packet_build(req, (uint16_t)sizeof(req), DHCP_TYPE_DISCOVER, &(dhcp_build_params_t){
                                                                                       .mac = mac,
                                                                                       .xid = cl.xid});
        CHECK(n > 0u);
        CHECK_U32(ts_deliver(&cl, req, n), 0);
    }
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_SELECTING);
    CHECK_U32(cl.candidate.address, 0u);
}

static void test_malformed_reply_is_counted_not_applied(void)
{
    dhcp_client_t cl;
    host_port_t hp;
    uint8_t mac[6];
    uint8_t buf[600];
    reply_spec_t s;
    uint16_t n;

    ts_setup(&cl, &hp, 13u);
    ts_mac(mac);
    CHECK(dhcp_client_start(&cl) == 0);

    ts_reply_default(&s, TS_OFFER, cl.xid, mac, k_server, k_addr, 600u);
    s.bad_opt_len = 200u;
    n = ts_build_reply(buf, (uint16_t)sizeof(buf), &s);
    CHECK(n > 0u);
    CHECK(ts_deliver(&cl, buf, n) < 0);
    CHECK_U32(cl.stats.rx_malformed, 1u);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_SELECTING);
    CHECK_U32(cl.candidate.address, 0u);
    CHECK_U32(cl.stats.rx_offer, 0u);

    /* truncated datagram: also rejected, nothing latched */
    ts_reply_default(&s, TS_OFFER, cl.xid, mac, k_server, k_addr, 600u);
    s.truncate_to = 250u;
    n = ts_build_reply(buf, (uint16_t)sizeof(buf), &s);
    CHECK(ts_deliver(&cl, buf, n) < 0);
    CHECK_U32(cl.stats.rx_malformed, 2u);
    CHECK_U32(cl.candidate.address, 0u);

    /* OFFER without a server identifier is not usable */
    ts_reply_default(&s, TS_OFFER, cl.xid, mac, k_server, k_addr, 600u);
    s.include_server = 0u;
    CHECK(ts_deliver_reply(&cl, &s) < 0);
    CHECK_U32(cl.candidate.address, 0u);
}

static void test_ack_for_other_address_rejected(void)
{
    dhcp_client_t cl;
    host_port_t hp;
    uint8_t mac[6];
    reply_spec_t s;

    ts_setup(&cl, &hp, 14u);
    ts_mac(mac);
    CHECK(dhcp_client_start(&cl) == 0);
    ts_reply_default(&s, TS_OFFER, cl.xid, mac, k_server, k_addr, 600u);
    CHECK_U32(ts_deliver_reply(&cl, &s), 1);
    run(&cl, &hp, 150u, 10u);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_REQUESTING);

    /* ACK binds a different address than the one we requested */
    ts_reply_default(&s, TS_ACK, cl.xid, mac, k_server, 0xC0A801FFu, 600u);
    CHECK_U32(ts_deliver_reply(&cl, &s), 0);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_REQUESTING);
    CHECK_U32(cl.lease.valid, 0u);

    /* ACK from a server we did not select */
    ts_reply_default(&s, TS_ACK, cl.xid, mac, 0xC0A801FEu, k_addr, 600u);
    CHECK_U32(ts_deliver_reply(&cl, &s), 0);
    CHECK_U32(cl.lease.valid, 0u);

    /* the right one still works */
    ts_reply_default(&s, TS_ACK, cl.xid, mac, k_server, k_addr, 600u);
    CHECK_U32(ts_deliver_reply(&cl, &s), 1);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_BOUND);
}

static void test_zero_lease_time_rejected(void)
{
    dhcp_client_t cl;
    host_port_t hp;
    uint8_t mac[6];
    reply_spec_t s;

    ts_setup(&cl, &hp, 15u);
    ts_mac(mac);
    CHECK(dhcp_client_start(&cl) == 0);
    ts_reply_default(&s, TS_OFFER, cl.xid, mac, k_server, k_addr, 600u);
    CHECK_U32(ts_deliver_reply(&cl, &s), 1);
    run(&cl, &hp, 150u, 10u);

    ts_reply_default(&s, TS_ACK, cl.xid, mac, k_server, k_addr, 0u);
    CHECK(ts_deliver_reply(&cl, &s) < 0);
    CHECK_U32(cl.stats.rx_malformed, 1u);
    CHECK_U32(cl.lease.valid, 0u);
}

/* ------------------------------------------------------------------------- */
/* Persistence / restore                                                     */
/* ------------------------------------------------------------------------- */

static void test_lease_blob_roundtrip(void)
{
    dhcp_lease_t in;
    dhcp_lease_t out;
    uint8_t blob[DHCP_LEASE_BLOB_LEN];
    uint16_t n;

    memset(&in, 0, sizeof(in));
    in.address = k_addr;
    in.subnet_mask = 0xFFFFFF00u;
    in.router = k_server;
    in.dns_count = 2u;
    in.dns[0] = 0x01010101u;
    in.dns[1] = 0x02020202u;
    in.server_id = k_server;
    in.lease_time_s = 86400u;
    in.t1_s = 43200u;
    in.t2_s = 75600u;
    in.valid = 1u;

    n = dhcp_lease_serialize(&in, blob, (uint16_t)sizeof(blob));
    CHECK_U32(n, DHCP_LEASE_BLOB_LEN);
    CHECK_U32(dhcp_lease_deserialize(blob, n, &out), 0);
    CHECK_U32(out.address, in.address);
    CHECK_U32(out.subnet_mask, in.subnet_mask);
    CHECK_U32(out.router, in.router);
    CHECK_U32(out.dns_count, 2u);
    CHECK_U32(out.dns[1], in.dns[1]);
    CHECK_U32(out.server_id, in.server_id);
    CHECK_U32(out.lease_time_s, in.lease_time_s);
    CHECK_U32(out.t1_s, in.t1_s);
    CHECK_U32(out.t2_s, in.t2_s);
    CHECK_U32(out.valid, 1u);

    /* corrupt one bit -> CRC must reject */
    blob[10] ^= 0x01u;
    CHECK(dhcp_lease_deserialize(blob, n, &out) < 0);
    blob[10] ^= 0x01u;

    /* truncated -> reject */
    CHECK(dhcp_lease_deserialize(blob, (uint16_t)(n - 1u), &out) < 0);
    /* foreign magic -> reject */
    blob[0] ^= 0xFFu;
    CHECK(dhcp_lease_deserialize(blob, n, &out) < 0);
}

static void test_restore_after_reboot_accepted(void)
{
    dhcp_client_t cl;
    host_port_t hp;
    uint8_t mac[6];
    dhcp_lease_t saved;
    reply_spec_t s;
    const host_tx_t *tx;

    ts_setup(&cl, &hp, 16u);
    ts_mac(mac);

    memset(&saved, 0, sizeof(saved));
    saved.address = k_addr;
    saved.subnet_mask = 0xFFFFFF00u;
    saved.router = k_server;
    saved.server_id = k_server;
    saved.lease_time_s = 3600u;
    saved.t1_s = 1800u;
    saved.t2_s = 3150u;
    saved.valid = 1u;

    CHECK(dhcp_client_restore(&cl, &saved) == 0);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_INIT_REBOOT);
    tx = host_last_tx(&hp);
    CHECK_U32(host_tx_msg_type(tx), DHCP_TYPE_REQUEST);
    CHECK_U32(tx_req_ip(tx), k_addr);   /* option 50 */
    CHECK_U32(tx_has_server_id(tx), 0u); /* no option 54 during INIT-REBOOT */

    ts_reply_default(&s, TS_ACK, cl.xid, mac, k_server, k_addr, 3600u);
    CHECK_U32(ts_deliver_reply(&cl, &s), 1);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_BOUND);
    CHECK_U32(ts_event_seen(DHCP_EVENT_RENEWED), 1);
}

static void test_restore_nak_falls_back_to_discover(void)
{
    dhcp_client_t cl;
    host_port_t hp;
    uint8_t mac[6];
    dhcp_lease_t saved;
    reply_spec_t s;

    ts_setup(&cl, &hp, 17u);
    ts_mac(mac);
    memset(&saved, 0, sizeof(saved));
    saved.address = k_addr;
    saved.server_id = k_server;
    saved.lease_time_s = 3600u;
    saved.valid = 1u;
    CHECK(dhcp_client_restore(&cl, &saved) == 0);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_INIT_REBOOT);

    ts_reply_default(&s, TS_NAK, cl.xid, mac, k_server, 0u, 0u);
    s.include_lease = 0u;
    s.include_mask = 0u;
    CHECK_U32(ts_deliver_reply(&cl, &s), 1);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_INIT);
    CHECK_U32(cl.lease.valid, 0u);

    run(&cl, &hp, 2500u, 100u);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_SELECTING);
}

static void test_restore_rejects_garbage(void)
{
    dhcp_client_t cl;
    host_port_t hp;
    dhcp_lease_t saved;

    ts_setup(&cl, &hp, 18u);
    memset(&saved, 0, sizeof(saved));
    saved.address = 0u;
    CHECK(dhcp_client_restore(&cl, &saved) < 0);
    CHECK(dhcp_client_restore(&cl, 0) < 0);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_INIT);
}

/* ------------------------------------------------------------------------- */
/* DECLINE / RELEASE / misc                                                  */
/* ------------------------------------------------------------------------- */

static void test_decline(void)
{
    dhcp_client_t cl;
    host_port_t hp;
    uint8_t mac[6];
    const host_tx_t *tx;

    ts_setup(&cl, &hp, 19u);
    ts_mac(mac);
    acquire(&cl, &hp, 600u);
    host_clear_tx(&hp);

    dhcp_client_decline(&cl);
    tx = host_last_tx(&hp);
    CHECK_U32(host_tx_msg_type(tx), DHCP_TYPE_DECLINE);
    CHECK_U32(tx_req_ip(tx), k_addr);
    CHECK_U32(ts_event_seen(DHCP_EVENT_DECLINED), 1);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_INIT);
    CHECK_U32(cl.lease.valid, 0u);
    CHECK(cl.stats.tx_decline >= 1u);

    run(&cl, &hp, 2500u, 100u);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_SELECTING);
}

static void test_release(void)
{
    dhcp_client_t cl;
    host_port_t hp;
    uint8_t mac[6];
    const host_tx_t *tx;

    ts_setup(&cl, &hp, 20u);
    ts_mac(mac);
    acquire(&cl, &hp, 600u);
    host_clear_tx(&hp);

    dhcp_client_release(&cl);
    tx = host_last_tx(&hp);
    CHECK_U32(host_tx_msg_type(tx), DHCP_TYPE_RELEASE);
    if (tx != 0) {
        CHECK_U32(tx->dst_ip, k_server);
        CHECK_U32(tx_ciaddr(tx), k_addr);
    }
    CHECK_U32(ts_event_seen(DHCP_EVENT_RELEASED), 1);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_STOPPED);
    CHECK_U32(cl.lease.valid, 0u);
    /* releasing erases the persisted lease */
    CHECK_U32(hp.nvm_written, 0);
}

static void test_next_timeout(void)
{
    dhcp_client_t cl;
    host_port_t hp;
    uint8_t mac[6];

    ts_setup(&cl, &hp, 21u);
    ts_mac(mac);
    acquire(&cl, &hp, 20u);

    /* BOUND with t1 = 10s: the next deadline must be about 10s away. */
    CHECK(dhcp_client_next_timeout_ms(&cl) <= 10000u);
    CHECK(dhcp_client_next_timeout_ms(&cl) > 9000u);

    run(&cl, &hp, 10500u, 50u);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_RENEWING);
    /* retransmission deadline is scheduled in the future */
    CHECK(dhcp_client_next_timeout_ms(&cl) <= 1000u);
}

static void test_stop_and_restart(void)
{
    dhcp_client_t cl;
    host_port_t hp;
    uint8_t mac[6];

    ts_setup(&cl, &hp, 22u);
    ts_mac(mac);
    acquire(&cl, &hp, 600u);
    dhcp_client_stop(&cl);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_STOPPED);
    /* stopped client does not retransmit anything */
    host_clear_tx(&hp);
    run(&cl, &hp, 20000u, 100u);
    CHECK_U32(hp.tx_count, 0u);

    CHECK(dhcp_client_start(&cl) == 0);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_SELECTING);
    CHECK_U32(host_count_type(&hp, DHCP_TYPE_DISCOVER), 1u);
}

static void test_tx_failure_is_retried(void)
{
    dhcp_client_t cl;
    host_port_t hp;

    ts_setup(&cl, &hp, 23u);
    hp.fail_tx = 1;
    CHECK_U32(dhcp_client_start(&cl), 0);
    CHECK_U32(cl.stats.tx_errors, 1u);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_SELECTING);

    hp.fail_tx = 0;
    run(&cl, &hp, 1200u, 50u);
    CHECK(cl.stats.tx_discover >= 1u);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_SELECTING);
}

/* ------------------------------------------------------------------------- */
/* Event ordering                                                            */
/* ------------------------------------------------------------------------- */

static void test_state_change_events(void)
{
    dhcp_client_t cl;
    host_port_t hp;
    static const dhcp_state_t expected[] = { DHCP_STATE_SELECTING, DHCP_STATE_REQUESTING,
                                            DHCP_STATE_BOUND };
    ts_events_t *ev;
    unsigned i;
    int bound_seen = 0;

    ts_setup(&cl, &hp, 30u);
    acquire(&cl, &hp, 600u);

    CHECK_U32(ts_state_change_count(), 3u);
    /* Every STATE_CHANGE must report old != new. */
    CHECK_U32(ts_state_change_inconsistent(), 0u);
    CHECK(ts_state_sequence_matches(expected, 3u) == 1);

    ev = ts_events();
    for (i = 0; i < ev->count; i++) {
        if (ev->events[i] == DHCP_EVENT_BOUND) {
            /* The event is delivered once the client is already BOUND. */
            CHECK_U32(ev->states[i], DHCP_STATE_BOUND);
            bound_seen = 1;
        }
        if (ev->events[i] == DHCP_EVENT_STATE_CHANGE) {
            CHECK(ev->old_states[i] != ev->states[i]);
        }
    }
    CHECK(bound_seen);
}

/* ------------------------------------------------------------------------- */
/* Strict ACK authentication                                                */
/* ------------------------------------------------------------------------- */

static void test_ack_without_server_id_rejected(void)
{
    dhcp_client_t cl;
    host_port_t hp;
    uint8_t mac[6];
    reply_spec_t s;

    ts_setup(&cl, &hp, 31u);
    ts_mac(mac);
    CHECK(dhcp_client_start(&cl) == 0);
    ts_reply_default(&s, TS_OFFER, cl.xid, mac, k_server, k_addr, 600u);
    CHECK_U32(ts_deliver_reply(&cl, &s), 1);
    run(&cl, &hp, 150u, 10u);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_REQUESTING);

    /* An ACK with the right address but no option 54 cannot be attributed to
     * the server we selected, so it must be ignored. */
    ts_reply_default(&s, TS_ACK, cl.xid, mac, k_server, k_addr, 600u);
    s.include_server = 0u;
    CHECK_U32(ts_deliver_reply(&cl, &s), 0);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_REQUESTING);
    CHECK_U32(cl.lease.valid, 0u);

    /* The properly authenticated ACK still completes the handshake. */
    ts_reply_default(&s, TS_ACK, cl.xid, mac, k_server, k_addr, 600u);
    CHECK_U32(ts_deliver_reply(&cl, &s), 1);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_BOUND);
}

static void test_renewing_rejects_foreign_server(void)
{
    dhcp_client_t cl;
    host_port_t hp;
    uint8_t mac[6];
    reply_spec_t s;

    ts_setup(&cl, &hp, 32u);
    ts_mac(mac);
    acquire(&cl, &hp, 20u);
    run(&cl, &hp, 10300u, 50u);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_RENEWING);

    /* ACK from a different server id, even for the right address. */
    ts_reply_default(&s, TS_ACK, cl.xid, mac, 0xC0A801FEu, k_addr, 20u);
    CHECK_U32(ts_deliver_reply(&cl, &s), 0);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_RENEWING);
    CHECK_U32(cl.lease.valid, 1u);
    CHECK_U32(cl.lease.server_id, k_server);

    /* ACK with no server id at all while renewing. */
    ts_reply_default(&s, TS_ACK, cl.xid, mac, k_server, k_addr, 20u);
    s.include_server = 0u;
    CHECK_U32(ts_deliver_reply(&cl, &s), 0);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_RENEWING);

    /* The real server is accepted. */
    ts_reply_default(&s, TS_ACK, cl.xid, mac, k_server, k_addr, 20u);
    CHECK_U32(ts_deliver_reply(&cl, &s), 1);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_BOUND);
}

static void test_rebinding_accepts_any_server(void)
{
    dhcp_client_t cl;
    host_port_t hp;
    uint8_t mac[6];
    reply_spec_t s;

    ts_setup(&cl, &hp, 33u);
    ts_mac(mac);
    acquire(&cl, &hp, 20u);
    run(&cl, &hp, 17500u, 50u);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_REBINDING);

    /* RFC 2131 4.3.2: after T2 any server may extend the lease. */
    ts_reply_default(&s, TS_ACK, cl.xid, mac, 0xC0A801FEu, k_addr, 20u);
    CHECK_U32(ts_deliver_reply(&cl, &s), 1);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_BOUND);
    CHECK_U32(cl.lease.server_id, 0xC0A801FEu); /* new server adopted */
}

/* ------------------------------------------------------------------------- */
/* Scheduler deadline                                                        */
/* ------------------------------------------------------------------------- */

static void test_next_timeout_includes_t2(void)
{
    dhcp_client_t cl;
    host_port_t hp;
    dhcp_client_cfg_t cfg;
    uint32_t nt;

    /* Retransmission timer deliberately longer than T2, so a correct
     * implementation must report the T2 deadline instead. */
    ts_default_cfg(&cfg);
    cfg.retry_base_ms = 10000u;
    cfg.retry_max_ms = 20000u;
    ts_setup_with(&cl, &hp, 34u, &cfg);

    acquire(&cl, &hp, 20u); /* t1 = 10 s, t2 = 17 s */
    run(&cl, &hp, 10400u, 50u);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_RENEWING);

    nt = dhcp_client_next_timeout_ms(&cl);
    /* ~7 s left to T2, not the 10 s retransmission deadline. */
    CHECK(nt <= 7000u);
    CHECK(nt >= 6000u);
}

/* ------------------------------------------------------------------------- */

void test_client_run_all(void)
{
    test_acquire();
    test_acquire_uses_server_t1_t2();
    test_acquire_rejects_bogus_t1_t2();
    test_retransmit_backoff_then_failed();
    test_offer_after_retransmit_still_works();
    test_request_timeout_restarts_discovery();
    test_nak_in_requesting();
    test_renew_at_t1();
    test_renew_timeout_goes_rebinding();
    test_lease_expiry();
    test_infinite_lease_never_renews();
    test_ignores_foreign_replies();
    test_malformed_reply_is_counted_not_applied();
    test_ack_for_other_address_rejected();
    test_zero_lease_time_rejected();
    test_lease_blob_roundtrip();
    test_restore_after_reboot_accepted();
    test_restore_nak_falls_back_to_discover();
    test_restore_rejects_garbage();
    test_decline();
    test_release();
    test_next_timeout();
    test_stop_and_restart();
    test_tx_failure_is_retried();
    test_state_change_events();
    test_ack_without_server_id_rejected();
    test_renewing_rejects_foreign_server();
    test_rebinding_accepts_any_server();
    test_next_timeout_includes_t2();
}
