/*
 * test_packet.c -- wire format: build, parse, and hostile input.
 */
#include "test_support.h"

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

static uint16_t build_discover(uint8_t *buf, uint16_t cap, uint32_t xid, const uint8_t mac[6])
{
    dhcp_build_params_t bp;

    memset(&bp, 0, sizeof(bp));
    bp.mac = mac;
    bp.xid = xid;
    bp.flags = DHCP_FLAG_BROADCAST;
    bp.hostname = "unit-test";
    bp.max_msg_size = (uint16_t)DHCP_MAX_MSG_SIZE;
    return dhcp_packet_build(buf, cap, DHCP_TYPE_DISCOVER, &bp);
}

/* Walk the option area by hand, independent of the parser under test. */
static int raw_find_option(const uint8_t *buf, uint16_t len, uint8_t code, const uint8_t **val,
                           uint8_t *vlen)
{
    uint16_t i = DHCP_MIN_MSG_LEN;

    while (i < len) {
        uint8_t c = buf[i++];
        uint8_t l;
        if (c == DHCP_OPT_PAD) {
            continue;
        }
        if (c == DHCP_OPT_END) {
            return 0;
        }
        if (i >= len) {
            return 0;
        }
        l = buf[i++];
        if ((uint32_t)i + l > len) {
            return 0;
        }
        if (c == code) {
            *val = &buf[i];
            *vlen = l;
            return 1;
        }
        i = (uint16_t)(i + l);
    }
    return 0;
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* ------------------------------------------------------------------------- */
/* Build                                                                     */
/* ------------------------------------------------------------------------- */

static void test_build_discover(void)
{
    uint8_t buf[DHCP_MAX_PAYLOAD_BYTES];
    uint8_t mac[6];
    const uint8_t *val = 0;
    uint8_t vlen = 0;
    uint16_t n;

    ts_mac(mac);
    n = build_discover(buf, (uint16_t)sizeof(buf), 0xDEADBEEFu, mac);

    CHECK(n >= 300u);
    CHECK(n <= sizeof(buf));
    CHECK_U32(buf[0], DHCP_OP_BOOTREQUEST);
    CHECK_U32(buf[1], DHCP_HTYPE_ETHERNET);
    CHECK_U32(buf[2], DHCP_HLEN_ETHERNET);
    CHECK_U32(buf[3], 0u);
    CHECK_U32(rd32(&buf[4]), 0xDEADBEEFu);
    CHECK_U32(buf[10], 0x80u); /* broadcast flag high byte */
    CHECK_U32(buf[28], mac[0]);
    CHECK_U32(rd32(&buf[DHCP_BOOTP_FIXED_LEN]), DHCP_MAGIC_COOKIE);

    /* message type must be the very first option */
    CHECK_U32(buf[DHCP_MIN_MSG_LEN], DHCP_OPT_MSG_TYPE);
    CHECK_U32(buf[DHCP_MIN_MSG_LEN + 1u], 1u);
    CHECK_U32(buf[DHCP_MIN_MSG_LEN + 2u], DHCP_TYPE_DISCOVER);

    /* client identifier = 1 + MAC */
    CHECK(raw_find_option(buf, n, DHCP_OPT_CLIENT_ID, &val, &vlen) == 1);
    CHECK_U32(vlen, 7u);
    if (val != 0) {
        CHECK_U32(val[0], 1u);
        CHECK_U32(val[1], mac[0]);
        CHECK_U32(val[6], mac[5]);
    }

    /* parameter request list present, empty requested-ip / server-id absent */
    CHECK(raw_find_option(buf, n, DHCP_OPT_PARAM_LIST, &val, &vlen) == 1);
    CHECK(vlen >= 5u);
    CHECK(raw_find_option(buf, n, DHCP_OPT_REQUESTED_IP, &val, &vlen) == 0);
    CHECK(raw_find_option(buf, n, DHCP_OPT_SERVER_ID, &val, &vlen) == 0);
}

static void test_build_request_options(void)
{
    uint8_t buf[DHCP_MAX_PAYLOAD_BYTES];
    uint8_t mac[6];
    dhcp_build_params_t bp;
    dhcp_msg_t msg;
    uint16_t n;

    ts_mac(mac);
    memset(&bp, 0, sizeof(bp));
    bp.mac = mac;
    bp.xid = 0x01020304u;
    bp.requested_ip = 0xC0A80164u; /* 192.168.1.100 */
    bp.server_id = 0xC0A80101u;    /* 192.168.1.1   */
    n = dhcp_packet_build(buf, (uint16_t)sizeof(buf), DHCP_TYPE_REQUEST, &bp);
    CHECK(n > 0u);

    CHECK(dhcp_packet_parse(buf, n, &msg) == DHCP_PARSE_OK);
    CHECK_U32(msg.msg_type, DHCP_TYPE_REQUEST);
    CHECK_U32(msg.has_requested_ip, 1u);
    CHECK_U32(msg.requested_ip, 0xC0A80164u);
    CHECK_U32(msg.has_server_id, 1u);
    CHECK_U32(msg.server_id, 0xC0A80101u);
}

static void test_build_rejects_bad_args(void)
{
    uint8_t buf[DHCP_MAX_PAYLOAD_BYTES];
    uint8_t mac[6];
    dhcp_build_params_t bp;

    ts_mac(mac);
    memset(&bp, 0, sizeof(bp));
    bp.mac = mac;

    CHECK_U32(dhcp_packet_build(0, 100u, DHCP_TYPE_DISCOVER, &bp), 0u);
    CHECK_U32(dhcp_packet_build(buf, 10u, DHCP_TYPE_DISCOVER, &bp), 0u);
    bp.mac = 0;
    CHECK_U32(dhcp_packet_build(buf, (uint16_t)sizeof(buf), DHCP_TYPE_DISCOVER, &bp), 0u);
    bp.mac = mac;
    CHECK_U32(dhcp_packet_build(buf, (uint16_t)sizeof(buf), 0u, &bp), 0u);
    CHECK_U32(dhcp_packet_build(buf, (uint16_t)sizeof(buf), 99u, &bp), 0u);
    /* exactly the minimum size still works */
    CHECK(dhcp_packet_build(buf, 270u, DHCP_TYPE_RELEASE, &bp) > 0u);
    /* one byte short of what a REQUEST needs is rejected cleanly */
    CHECK_U32(dhcp_packet_build(buf, DHCP_MIN_MSG_LEN, DHCP_TYPE_RELEASE, &bp), 0u);
}

/* ------------------------------------------------------------------------- */
/* Parse: happy path                                                         */
/* ------------------------------------------------------------------------- */

static void test_parse_full_ack(void)
{
    reply_spec_t s;
    uint8_t buf[600];
    uint8_t mac[6];
    dhcp_msg_t msg;
    uint16_t n;

    ts_mac(mac);
    ts_reply_default(&s, TS_ACK, 0x11223344u, mac, 0xC0A80101u, 0xC0A80164u, 43200u);
    s.include_t1t2 = 1u;
    s.t1 = 21600u;
    s.t2 = 37800u;
    n = ts_build_reply(buf, (uint16_t)sizeof(buf), &s);
    CHECK(n > 0u);

    CHECK_U32(dhcp_packet_parse(buf, n, &msg), DHCP_PARSE_OK);
    CHECK_U32(msg.op, DHCP_OP_BOOTREPLY);
    CHECK_U32(msg.xid, 0x11223344u);
    CHECK_U32(msg.msg_type, TS_ACK);
    CHECK_U32(msg.yiaddr, 0xC0A80164u);
    CHECK_U32(msg.has_subnet_mask, 1u);
    CHECK_U32(msg.subnet_mask, 0xFFFFFF00u);
    CHECK_U32(msg.router_count, 1u);
    CHECK_U32(msg.router[0], 0xC0A80101u);
    CHECK_U32(msg.dns_count, 2u);
    CHECK_U32(msg.dns[0], 0x08080808u);
    CHECK_U32(msg.dns[1], 0x08080404u);
    CHECK_U32(msg.has_lease_time, 1u);
    CHECK_U32(msg.lease_time_s, 43200u);
    CHECK_U32(msg.has_server_id, 1u);
    CHECK_U32(msg.server_id, 0xC0A80101u);
    CHECK_U32(msg.has_t1, 1u);
    CHECK_U32(msg.t1_s, 21600u);
    CHECK_U32(msg.has_t2, 1u);
    CHECK_U32(msg.t2_s, 37800u);
    CHECK(memcmp(msg.chaddr, mac, 6u) == 0);
}

static void test_parse_infinite_lease(void)
{
    reply_spec_t s;
    uint8_t buf[600];
    uint8_t mac[6];
    dhcp_msg_t msg;
    uint16_t n;

    ts_mac(mac);
    ts_reply_default(&s, TS_ACK, 7u, mac, 0xC0A80101u, 0xC0A80164u, DHCP_INFINITE_LEASE_S);
    n = ts_build_reply(buf, (uint16_t)sizeof(buf), &s);
    CHECK_U32(dhcp_packet_parse(buf, n, &msg), DHCP_PARSE_OK);
    CHECK_U32(msg.lease_time_s, DHCP_INFINITE_LEASE_S);
}

/* ------------------------------------------------------------------------- */
/* Parse: malformed input                                                    */
/* ------------------------------------------------------------------------- */

static void test_parse_rejects_malformed(void)
{
    reply_spec_t s;
    uint8_t buf[600];
    uint8_t mac[6];
    dhcp_msg_t msg;
    uint16_t n;

    ts_mac(mac);

    ts_reply_default(&s, TS_ACK, 1u, mac, 0xC0A80101u, 0xC0A80164u, 3600u);
    n = ts_build_reply(buf, (uint16_t)sizeof(buf), &s);
    CHECK(n > 240u);
    CHECK_U32(dhcp_packet_parse(buf, n, &msg), DHCP_PARSE_OK);

    /* 1. shorter than the fixed BOOTP part */
    CHECK_U32(dhcp_packet_parse(buf, 239u, &msg), DHCP_PARSE_ERR_TOO_SHORT);
    CHECK_U32(dhcp_packet_parse(buf, 0u, &msg), DHCP_PARSE_ERR_TOO_SHORT);

    /* 2. NULL args */
    CHECK_U32(dhcp_packet_parse(0, n, &msg), DHCP_PARSE_ERR_NULL_ARG);
    CHECK_U32(dhcp_packet_parse(buf, n, 0), DHCP_PARSE_ERR_NULL_ARG);

    /* 3. bad op */
    ts_reply_default(&s, TS_ACK, 1u, mac, 0xC0A80101u, 0xC0A80164u, 3600u);
    s.op = 7u;
    n = ts_build_reply(buf, (uint16_t)sizeof(buf), &s);
    CHECK_U32(dhcp_packet_parse(buf, n, &msg), DHCP_PARSE_ERR_BAD_OP);

    /* 4. bad hlen */
    ts_reply_default(&s, TS_ACK, 1u, mac, 0xC0A80101u, 0xC0A80164u, 3600u);
    s.hlen = 200u;
    n = ts_build_reply(buf, (uint16_t)sizeof(buf), &s);
    CHECK_U32(dhcp_packet_parse(buf, n, &msg), DHCP_PARSE_ERR_BAD_HLEN);

    /* 5. missing / wrong magic cookie */
    ts_reply_default(&s, TS_ACK, 1u, mac, 0xC0A80101u, 0xC0A80164u, 3600u);
    s.no_cookie = 1u;
    n = ts_build_reply(buf, (uint16_t)sizeof(buf), &s);
    CHECK_U32(dhcp_packet_parse(buf, n, &msg), DHCP_PARSE_ERR_BAD_COOKIE);

    /* 6. option that claims more bytes than the datagram holds */
    ts_reply_default(&s, TS_ACK, 1u, mac, 0xC0A80101u, 0xC0A80164u, 3600u);
    s.bad_opt_len = 200u;
    n = ts_build_reply(buf, (uint16_t)sizeof(buf), &s);
    CHECK_U32(dhcp_packet_parse(buf, n, &msg), DHCP_PARSE_ERR_OPTIONS);

    /* 7. no message type option */
    ts_reply_default(&s, TS_ACK, 1u, mac, 0xC0A80101u, 0xC0A80164u, 3600u);
    s.no_msg_type = 1u;
    n = ts_build_reply(buf, (uint16_t)sizeof(buf), &s);
    CHECK_U32(dhcp_packet_parse(buf, n, &msg), DHCP_PARSE_ERR_NO_TYPE);
}

/* Every truncation length must be handled without reading out of bounds. */
static void test_parse_all_truncations(void)
{
    reply_spec_t s;
    uint8_t buf[600];
    uint8_t mac[6];
    dhcp_msg_t msg;
    uint16_t full;
    uint16_t cut;
    int ok_at_full = 0;

    ts_mac(mac);
    ts_reply_default(&s, TS_ACK, 1u, mac, 0xC0A80101u, 0xC0A80164u, 3600u);
    s.include_t1t2 = 1u;
    full = ts_build_reply(buf, (uint16_t)sizeof(buf), &s);
    CHECK(full > 240u);

    for (cut = 0; cut < full; cut++) {
        dhcp_parse_result_t r = dhcp_packet_parse(buf, cut, &msg);
        /* Must never report success for a datagram shorter than the fixed
         * BOOTP part, and never crash for any other truncation. */
        if (r == DHCP_PARSE_OK && cut < DHCP_MIN_MSG_LEN) {
            ok_at_full++;
        }
    }
    CHECK_U32(ok_at_full, 0u);
    /* A cut inside an option body must always be detected. */
    CHECK(dhcp_packet_parse(buf, 245u, &msg) != DHCP_PARSE_OK);
    CHECK(dhcp_packet_parse(buf, 251u, &msg) != DHCP_PARSE_OK);
    CHECK_U32(dhcp_packet_parse(buf, full, &msg), DHCP_PARSE_OK);
}

/* Random mutations must never crash or over-read (run under ASAN too). */
static void test_parse_fuzz(void)
{
    reply_spec_t s;
    uint8_t buf[600];
    uint8_t mac[6];
    dhcp_msg_t msg;
    uint16_t base;
    uint32_t lcg = 0x12345678u;
    unsigned iter;
    unsigned ok = 0u;

    ts_mac(mac);
    ts_reply_default(&s, TS_ACK, 1u, mac, 0xC0A80101u, 0xC0A80164u, 3600u);
    s.include_t1t2 = 1u;
    base = ts_build_reply(buf, (uint16_t)sizeof(buf), &s);

    for (iter = 0; iter < 4000u; iter++) {
        uint8_t work[600];
        uint16_t len = base;
        unsigned edits;
        unsigned e;

        lcg = lcg * 1103515245u + 12345u;
        memcpy(work, buf, base);
        edits = 1u + (lcg >> 16) % 8u;
        for (e = 0; e < edits; e++) {
            uint16_t pos;
            lcg = lcg * 1103515245u + 12345u;
            pos = (uint16_t)((lcg >> 8) % base);
            lcg = lcg * 1103515245u + 12345u;
            work[pos] = (uint8_t)(lcg >> 12);
        }
        lcg = lcg * 1103515245u + 12345u;
        len = (uint16_t)(base - ((lcg >> 16) % 40u));

        if (dhcp_packet_parse(work, len, &msg) == DHCP_PARSE_OK) {
            ok++;
            /* Whatever came back must be self consistent. */
            CHECK(msg.hlen <= 16u);
            CHECK(msg.dns_count <= (uint8_t)DHCP_MAX_DNS);
            CHECK(msg.router_count <= (uint8_t)DHCP_MAX_ROUTERS);
        }
    }
    /* Most mutants should still parse (only a few bytes differ), but the point
     * of the test is that nothing crashed. */
    CHECK(ok > 0u);
}

/* Options with an invalid length must be ignored, not "fixed up". */
static void test_parse_ignores_bad_lengths(void)
{
    uint8_t buf[600];
    uint8_t mac[6];
    dhcp_msg_t msg;
    uint16_t n;
    uint16_t i;

    ts_mac(mac);
    memset(buf, 0, sizeof(buf));
    /* Hand crafted reply: mask option with length 3 (invalid). */
    buf[0] = DHCP_OP_BOOTREPLY;
    buf[1] = 1u;
    buf[2] = 6u;
    buf[4] = 0u;
    buf[5] = 0u;
    buf[6] = 0u;
    buf[7] = 9u;
    memcpy(&buf[28], mac, 6u);
    buf[236] = 0x63u;
    buf[237] = 0x82u;
    buf[238] = 0x53u;
    buf[239] = 0x63u;
    i = DHCP_MIN_MSG_LEN;
    buf[i++] = DHCP_OPT_MSG_TYPE;
    buf[i++] = 1u;
    buf[i++] = TS_ACK;
    buf[i++] = DHCP_OPT_SUBNET_MASK;
    buf[i++] = 3u; /* wrong length */
    buf[i++] = 0xFFu;
    buf[i++] = 0xFFu;
    buf[i++] = 0xFFu;
    buf[i++] = DHCP_OPT_LEASE_TIME;
    buf[i++] = 4u;
    buf[i++] = 0u;
    buf[i++] = 0u;
    buf[i++] = 0x0Eu;
    buf[i++] = 0x10u;
    buf[i++] = DHCP_OPT_END;
    n = i;

    CHECK_U32(dhcp_packet_parse(buf, n, &msg), DHCP_PARSE_OK);
    CHECK_U32(msg.has_subnet_mask, 0u);
    CHECK_U32(msg.has_lease_time, 1u);
    CHECK_U32(msg.lease_time_s, 3600u);
}

/* ------------------------------------------------------------------------- */

void test_packet_run_all(void)
{
    test_build_discover();
    test_build_request_options();
    test_build_rejects_bad_args();
    test_parse_full_ack();
    test_parse_infinite_lease();
    test_parse_rejects_malformed();
    test_parse_all_truncations();
    test_parse_fuzz();
    test_parse_ignores_bad_lengths();
}
