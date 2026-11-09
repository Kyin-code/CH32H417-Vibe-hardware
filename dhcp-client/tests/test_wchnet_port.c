/*
 * test_wchnet_port.c -- executes ports/ch32h417/dhcp_port_wchnet.c on the host
 * against a fake WCHNET stack (ports/ch32h417/sim).
 *
 * Built twice, once with DHCP_WCHNET_USE_UDP_SOCKET=1 and once with =0, so both
 * transport paths are really executed instead of only type-checked.
 *
 * The weak hooks of the port (apply_lease / lease_save / lease_load /
 * lease_clear) are overridden here with strong definitions so the test can see
 * what the port hands to the application.
 */
#include "test_support.h"
#include "dhcp_port_wchnet.h"
#include "wchnet_sim.h"

#define SERVER_IP 0xC0A80101u
#define LEASED_IP 0xC0A80164u
#define OTHER_IP 0xC0A801FEu

static unsigned g_apply_calls;
static unsigned g_save_calls;
static unsigned g_clear_calls;
static dhcp_lease_t g_applied;

/* --- strong overrides of the port's weak platform hooks ------------------ */

void dhcp_wchnet_apply_lease(const dhcp_lease_t *lease)
{
    g_apply_calls++;
    g_applied = *lease;
}

void dhcp_wchnet_lease_save(const uint8_t *blob, uint16_t len)
{
    (void)blob;
    (void)len;
    g_save_calls++;
}

int dhcp_wchnet_lease_load(uint8_t *blob, uint16_t cap, uint16_t *len)
{
    (void)blob;
    (void)cap;
    (void)len;
    return -1; /* nothing persisted in the simulator */
}

void dhcp_wchnet_lease_clear(void)
{
    g_clear_calls++;
}

/* ------------------------------------------------------------------------- */

static dhcp_wchnet_ctx_t ctx;
static dhcp_client_t cl;
static uint8_t g_mac[6];

static void setup(void)
{
    wchnet_sim_reset();
    ts_events_reset();
    g_apply_calls = 0u;
    g_save_calls = 0u;
    g_clear_calls = 0u;
    memset(&g_applied, 0, sizeof(g_applied));
    ts_mac(g_mac);
    CHECK(dhcp_wchnet_init(&ctx, &cl, g_mac, "wchnet-sim") == 0);
}

static int sock_id(void)
{
    return wchnet_sim_socket_id();
}

/* DHCP message type of the most recent transmission, skipping the IP_RAW UDP
 * header when that transport is in use. */
static uint8_t last_msg_type(void)
{
    const sim_tx_t *tx = wchnet_sim_last_tx();
    dhcp_msg_t msg;
    uint16_t off = 0u;

    if (tx == 0) {
        return 0u;
    }
#if !DHCP_WCHNET_USE_UDP_SOCKET
    off = 8u;
#endif
    if (tx->len <= off) {
        return 0u;
    }
    if (dhcp_packet_parse(&tx->data[off], (uint16_t)(tx->len - off), &msg) != DHCP_PARSE_OK) {
        return 0u;
    }
    return msg.msg_type;
}

/* Advance the port's clock and let the state machine catch up.
 *
 * A single poll only walks one state transition, so a big jump that crosses
 * both T1 and T2 needs a few polls -- exactly like a main loop that was busy
 * for a while.
 */
static void advance(uint32_t ms)
{
    unsigned i;

    dhcp_wchnet_tick(&ctx, ms);
    for (i = 0u; i < 4u; i++) {
        dhcp_client_poll(&cl);
    }
}

static void deliver(uint8_t type, uint32_t addr, uint32_t lease_s)
{
    reply_spec_t s;
    uint8_t buf[600];
    uint16_t n;

    ts_reply_default(&s, type, cl.xid, g_mac, SERVER_IP, addr, lease_s);
    n = ts_build_reply(buf, (uint16_t)sizeof(buf), &s);
    CHECK(n > 0u);
    CHECK(wchnet_sim_inject((uint8_t)sock_id(), buf, n) == 0);
    dhcp_wchnet_poll(&ctx);
    dhcp_client_poll(&cl);
}

/* Drive to BOUND with a 20 s lease (t1 = 10 s, t2 = 17 s). */
static void acquire(uint32_t lease_s)
{
    CHECK(dhcp_wchnet_start(&ctx) == 0);
    deliver(TS_OFFER, LEASED_IP, lease_s);
    CHECK_U32(cl.stats.rx_offer, 1u);

    dhcp_wchnet_tick(&ctx, 1100u); /* past the 1000 ms OFFER collection window */
    dhcp_client_poll(&cl);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_REQUESTING);
    CHECK_U32(last_msg_type(), DHCP_TYPE_REQUEST);
    deliver(TS_ACK, LEASED_IP, lease_s);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_BOUND);
}

/* ------------------------------------------------------------------------- */
/* Socket setup and DISCOVER framing                                         */
/* ------------------------------------------------------------------------- */

static void test_socket_setup(void)
{
    const sim_stats_t *st;

    setup();
    st = wchnet_sim_stats();
    CHECK_U32(st->creat_calls, 0u); /* init must not open a socket yet */

    CHECK(dhcp_wchnet_start(&ctx) == 0);
    CHECK_U32(st->creat_calls, 1u);

#if DHCP_WCHNET_USE_UDP_SOCKET
    CHECK_U32(st->proto, PROTO_TYPE_UDP);
    CHECK_U32(st->sour_port, DHCP_CLIENT_PORT);
    CHECK_U32(st->des_port, DHCP_SERVER_PORT);
#else
    CHECK_U32(st->proto, PROTO_TYPE_IP_RAW);
    CHECK_U32(st->sour_port, 17u); /* UDP protocol number */
#endif
#if DHCP_WCHNET_USE_UDP_SOCKET
    /* UDP: the PCB must stay UNCONNECTED (0.0.0.0), otherwise WCHNET_SocketCreat
     * calls udp_connect() and lwIP filters out the server's unicast reply. */
    CHECK_U32(st->dst_ip[0], 0u);
    CHECK_U32(st->dst_ip[3], 0u);
#else
    /* IP_RAW is connection oriented, so the broadcast address is baked in. */
    CHECK_U32(st->dst_ip[0], 255u);
    CHECK_U32(st->dst_ip[3], 255u);
#endif
    CHECK_U32(st->recv_buf_len, DHCP_WCHNET_RX_BUF_LEN);
    CHECK(st->recv_start != 0u);
    /* The receive buffer must be attached to the socket right after creation,
     * otherwise the socket can send but never receives (real-hardware bug: the
     * DHCP offer was silently dropped, counters showed "d=5 off=0"). */
    CHECK_U32(st->modifyrecvbuf_calls, 1u);
    CHECK_U32(st->modify_recv_addr, st->recv_start);
    CHECK_U32(st->modify_recv_len, DHCP_WCHNET_RX_BUF_LEN);

    CHECK_U32(last_msg_type(), DHCP_TYPE_DISCOVER);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_SELECTING);

    /* The port must own exactly its own socket, so the vendor's global-int
     * handler can skip it (otherwise DataLoopback would eat our replies). */
    CHECK_U32(dhcp_wchnet_owns_socket(&ctx, (uint8_t)sock_id()), 1u);
    CHECK_U32(dhcp_wchnet_owns_socket(&ctx, (uint8_t)(sock_id() ^ 1)), 0u);
    CHECK_U32(dhcp_wchnet_owns_socket(0, 0u), 0u);
}

static void test_discover_framing(void)
{
    const sim_tx_t *tx;

    setup();
    CHECK(dhcp_wchnet_start(&ctx) == 0);
    tx = wchnet_sim_last_tx();
    CHECK(tx != 0);
    if (tx == 0) {
        return;
    }

#if DHCP_WCHNET_USE_UDP_SOCKET
    /* The stack builds IP/UDP; the port hands over the bare DHCP payload. */
    CHECK_U32(tx->via, 1u);
    CHECK_U32(tx->dst_port, DHCP_SERVER_PORT);
    CHECK_U32(tx->dst_ip[0], 255u);
    CHECK_U32(tx->dst_ip[3], 255u);
    CHECK_U32(tx->data[0], DHCP_OP_BOOTREQUEST);
#else
    /* IP_RAW: the port prepends an 8 byte UDP header itself. */
    CHECK_U32(tx->via, 0u);
    CHECK_U32(tx->data[0], 0u);
    CHECK_U32(tx->data[1], DHCP_CLIENT_PORT);
    CHECK_U32(tx->data[2], 0u);
    CHECK_U32(tx->data[3], DHCP_SERVER_PORT);
    CHECK_U32(((unsigned)tx->data[4] << 8) | tx->data[5], tx->len); /* UDP len */
    CHECK_U32(tx->data[6], 0u); /* checksum disabled */
    CHECK_U32(tx->data[7], 0u);
    CHECK_U32(tx->data[8], DHCP_OP_BOOTREQUEST);
#endif
    /* Sanity: the payload is a well formed DHCP DISCOVER. */
    CHECK_U32(last_msg_type(), DHCP_TYPE_DISCOVER);
}

/* ------------------------------------------------------------------------- */
/* Receive path                                                              */
/* ------------------------------------------------------------------------- */

static void test_receive_plain_payload(void)
{
    const sim_stats_t *st;

    setup();
    CHECK(dhcp_wchnet_start(&ctx) == 0);
    st = wchnet_sim_stats();

    deliver(TS_OFFER, LEASED_IP, 600u);
    CHECK_U32(cl.stats.rx_offer, 1u);
    CHECK_U32(cl.stats.rx_malformed, 0u);
    /* The port must clear the socket interrupt and drain the queue. */
    CHECK(st->getint_calls >= 1u);
    CHECK_U32(st->recv_calls, 1u);
    /* Reading it again must return 0: the bit was consumed by the port. */
    CHECK_U32(WCHNET_GetSocketInt((uint8_t)sock_id()), 0u);
}

static void test_receive_with_udp_header(void)
{
    reply_spec_t s;
    uint8_t payload[600];
    uint8_t framed[608];
    uint16_t n;
    uint16_t ulen;

    setup();
    CHECK(dhcp_wchnet_start(&ctx) == 0);

    ts_reply_default(&s, TS_OFFER, cl.xid, g_mac, SERVER_IP, LEASED_IP, 600u);
    n = ts_build_reply(payload, (uint16_t)sizeof(payload), &s);
    CHECK(n > 0u);

    /* Simulate a stack that keeps the UDP header (IP_RAW mode, or a UDP socket
     * implementation that does not strip it): 68 -> 67 with matching length. */
    framed[0] = 0u;
    framed[1] = DHCP_SERVER_PORT;
    framed[2] = 0u;
    framed[3] = DHCP_CLIENT_PORT;
    ulen = (uint16_t)(n + 8u);
    framed[4] = (uint8_t)(ulen >> 8);
    framed[5] = (uint8_t)ulen;
    framed[6] = 0u;
    framed[7] = 0u;
    memcpy(&framed[8], payload, n);
    CHECK(wchnet_sim_inject((uint8_t)sock_id(), framed, (uint16_t)(n + 8u)) == 0);

    dhcp_wchnet_poll(&ctx);
    CHECK_U32(cl.stats.rx_offer, 1u);
    CHECK_U32(cl.stats.rx_malformed, 0u);
}

static void test_receive_garbage_is_safe(void)
{
    uint8_t junk[600];
    unsigned i;

    setup();
    CHECK(dhcp_wchnet_start(&ctx) == 0);

    /* random-ish garbage */
    for (i = 0; i < sizeof(junk); i++) {
        junk[i] = (uint8_t)(i * 37u);
    }
    CHECK(wchnet_sim_inject((uint8_t)sock_id(), junk, (uint16_t)sizeof(junk)) == 0);
    dhcp_wchnet_poll(&ctx);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_SELECTING);
    CHECK(cl.stats.rx_malformed >= 1u);

    /* a well formed reply that is one byte short of the option area */
    {
        reply_spec_t s;
        uint8_t buf[600];
        uint16_t n;

        ts_reply_default(&s, TS_OFFER, cl.xid, g_mac, SERVER_IP, LEASED_IP, 600u);
        s.truncate_to = 250u;
        n = ts_build_reply(buf, (uint16_t)sizeof(buf), &s);
        CHECK(n > 0u);
        CHECK(wchnet_sim_inject((uint8_t)sock_id(), buf, n) == 0);
        dhcp_wchnet_poll(&ctx);
        CHECK_U32(cl.stats.rx_offer, 0u);
        CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_SELECTING);
    }
}

static void test_oversized_datagram_truncated(void)
{
    /* Send more than the port's RX buffer. The port must drop the whole
     * datagram (a split DHCP message is useless) instead of feeding the
     * parser two halves. */
    static uint8_t big[1200];
    unsigned i;

    setup();
    CHECK(dhcp_wchnet_start(&ctx) == 0);
    for (i = 0; i < sizeof(big); i++) {
        big[i] = 0xAAu;
    }
    CHECK(wchnet_sim_inject((uint8_t)sock_id(), big, (uint16_t)sizeof(big)) == 0);
    dhcp_wchnet_poll(&ctx);

    /* Queue drained, nothing handed to the DHCP core. */
    CHECK_U32(wchnet_sim_rx_pending((uint8_t)sock_id()), 0u);
    CHECK_U32(cl.stats.rx_malformed, 0u);
    CHECK_U32(cl.stats.rx_ignored, 0u);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_SELECTING);
    CHECK_U32(cl.lease.valid, 0u);
}

static void test_partial_send_is_reported_as_failure(void)
{
    /* The vendor send APIs are in/out: a short write means only part of the
     * datagram left the chip. The port must report that as an error so the
     * client retransmits a complete message. */
    setup();
    wchnet_sim_set_partial_send(10u);
    CHECK(dhcp_wchnet_start(&ctx) == 0);

    CHECK_U32(cl.stats.tx_errors, 1u);
    CHECK_U32(cl.stats.tx_discover, 0u); /* not counted as sent */
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_SELECTING);

    /* Once the driver accepts full datagrams again, retransmission works. */
    wchnet_sim_set_partial_send(0u);
    dhcp_wchnet_tick(&ctx, 4100u); /* past retry_base_ms (4000) */
    dhcp_client_poll(&cl);
    CHECK_U32(cl.stats.tx_discover, 1u);
    CHECK_U32(last_msg_type(), DHCP_TYPE_DISCOVER);
}

/* ------------------------------------------------------------------------- */
/* Full handshake through the port                                           */
/* ------------------------------------------------------------------------- */

static void test_handshake_and_apply_lease(void)
{
    const dhcp_lease_t *lease;

    setup();
    acquire(600u);

    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_BOUND);
    CHECK_U32(g_apply_calls, 1u); /* port reported the lease to the app once */
    CHECK_U32(g_applied.address, LEASED_IP);
    CHECK_U32(g_applied.server_id, SERVER_IP);
    CHECK(g_save_calls >= 1u); /* lease persisted through the weak hook */

    lease = dhcp_wchnet_lease(&ctx);
    CHECK(lease != 0);
    if (lease != 0) {
        CHECK_U32(lease->address, LEASED_IP);
        CHECK_U32(lease->valid, 1u);
    }
}

static void test_renew_uses_unicast(void)
{
    const sim_tx_t *tx;
    const sim_stats_t *st;

    setup();
    acquire(20u); /* t1 = 10 s */
    st = wchnet_sim_stats();
    CHECK_U32(g_apply_calls, 1u);

    dhcp_wchnet_tick(&ctx, 10500u);
    dhcp_client_poll(&cl);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_RENEWING);
    tx = wchnet_sim_last_tx();
    CHECK(tx != 0);
    CHECK_U32(last_msg_type(), DHCP_TYPE_REQUEST);
    if (tx != 0) {
#if DHCP_WCHNET_USE_UDP_SOCKET
        /* Unicast target chosen per datagram; the socket is not rebuilt. */
        CHECK_U32(tx->dst_ip[0], 192u);
        CHECK_U32(tx->dst_ip[3], 1u);
        CHECK_U32(st->creat_calls, 1u);
#else
        /* IP_RAW binds the destination to the socket, so the port must rebuild
         * it for the unicast target. */
        CHECK_U32(st->close_calls, 1u);
        CHECK_U32(st->creat_calls, 2u);
        CHECK_U32(st->dst_ip[0], 192u);
        CHECK_U32(st->dst_ip[3], 1u);
        CHECK_U32(tx->via, 0u);
#endif
    }

    /* The renewal ACK is applied again. */
    deliver(TS_ACK, LEASED_IP, 20u);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_BOUND);
    CHECK_U32(g_apply_calls, 2u);
}

static void test_rebinding_broadcasts_again(void)
{
    const sim_tx_t *tx;

    setup();
    acquire(20u);

    /* Past T2 (17 s): REBINDING, broadcast again. */
    advance(18000u);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_REBINDING);

    tx = wchnet_sim_last_tx();
    CHECK(tx != 0);
    if (tx != 0) {
#if DHCP_WCHNET_USE_UDP_SOCKET
        CHECK_U32(tx->dst_ip[0], 255u); /* broadcast via SendTo */
#else
        CHECK_U32(wchnet_sim_stats()->dst_ip[0], 255u); /* socket re-targeted */
#endif
    }
    /* A rebinding ACK from another server is accepted. */
    {
        reply_spec_t s;
        uint8_t buf[600];
        uint16_t n;

        ts_reply_default(&s, TS_ACK, cl.xid, g_mac, OTHER_IP, LEASED_IP, 20u);
        n = ts_build_reply(buf, (uint16_t)sizeof(buf), &s);
        CHECK(n > 0u);
        CHECK(wchnet_sim_inject((uint8_t)sock_id(), buf, n) == 0);
        dhcp_wchnet_poll(&ctx);
        dhcp_client_poll(&cl);
        CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_BOUND);
        CHECK_U32(cl.lease.server_id, OTHER_IP);
    }
}

static void test_poll_before_start_is_safe(void)
{
    setup();
    /* No socket yet: poll must be a no-op, not a crash. */
    dhcp_wchnet_poll(&ctx);
    dhcp_client_poll(&cl);
    CHECK_U32(dhcp_client_get_state(&cl), DHCP_STATE_INIT);
    CHECK_U32(wchnet_sim_stats()->recvlen_calls, 0u);
}

int main(void)
{
    printf("dhcp-client WCHNET port tests (%s transport)\n",
#if DHCP_WCHNET_USE_UDP_SOCKET
           "UDP socket"
#else
           "IP_RAW"
#endif
    );

    test_socket_setup();
    test_discover_framing();
    test_receive_plain_payload();
    test_receive_with_udp_header();
    test_receive_garbage_is_safe();
    test_oversized_datagram_truncated();
    test_partial_send_is_reported_as_failure();
    test_handshake_and_apply_lease();
    test_renew_uses_unicast();
    test_rebinding_broadcasts_again();
    test_poll_before_start_is_safe();

    printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
