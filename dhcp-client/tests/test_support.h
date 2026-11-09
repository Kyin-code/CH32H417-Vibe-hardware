#ifndef TEST_SUPPORT_H
#define TEST_SUPPORT_H

#include "dhcp_client.h"
#include "dhcp_port_host.h"

#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------- */
/* Minimal test harness (no external dependencies)                        */
/* ---------------------------------------------------------------------- */

extern int g_checks;
extern int g_failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failures++;                                                        \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                        \
    } while (0)

#define CHECK_MSG(cond, msg)                                                     \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failures++;                                                        \
            printf("  FAIL %s:%d  %s (%s)\n", __FILE__, __LINE__, #cond, (msg)); \
        }                                                                        \
    } while (0)

#define CHECK_U32(actual, expected)                                              \
    do {                                                                         \
        unsigned long a_ = (unsigned long)(actual);                              \
        unsigned long e_ = (unsigned long)(expected);                            \
        g_checks++;                                                              \
        if (a_ != e_) {                                                          \
            g_failures++;                                                        \
            printf("  FAIL %s:%d  %s == 0x%lx, expected 0x%lx\n", __FILE__,      \
                   __LINE__, #actual, a_, e_);                                   \
        }                                                                        \
    } while (0)

#define TS_ASSERT_EQ(actual, expected) CHECK_U32((actual), (expected))

/* ---------------------------------------------------------------------- */
/* Fake DHCP server: builds BOOTREPLY datagrams for the tests             */
/* ---------------------------------------------------------------------- */

typedef struct {
    uint8_t msg_type; /* DHCP_TYPE_OFFER / ACK / NAK ... */
    uint8_t op;       /* 0 => BOOTREPLY */
    uint8_t hlen;     /* 0 => 6 */
    uint32_t xid;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint16_t flags;
    const uint8_t *mac; /* echoed chaddr, NULL => all zero */

    /* option toggles */
    uint8_t include_server;
    uint8_t include_mask;
    uint8_t include_router;
    uint8_t include_dns2;
    uint8_t include_lease;
    uint8_t include_t1t2;

    uint32_t server_id;
    uint32_t mask;
    uint32_t router;
    uint32_t dns1;
    uint32_t dns2;
    uint32_t lease_time;
    uint32_t t1;
    uint32_t t2;

    /* fault injection */
    uint8_t no_cookie;      /* write a wrong magic cookie            */
    uint8_t no_msg_type;    /* skip option 53                        */
    uint8_t bad_opt_len;    /* append an option claiming this length  */
    uint16_t truncate_to;   /* return fewer bytes than written        */
} reply_spec_t;

#define TS_OFFER 2u
#define TS_ACK 5u
#define TS_NAK 6u

/** Defaults for a well formed reply from a server. */
void ts_reply_default(reply_spec_t *s, uint8_t type, uint32_t xid, const uint8_t mac[6],
                      uint32_t server_id, uint32_t addr, uint32_t lease_time);

/** Serialise the spec. Returns the number of bytes written (0 on overflow). */
uint16_t ts_build_reply(uint8_t *buf, uint16_t cap, const reply_spec_t *s);

/* ---------------------------------------------------------------------- */
/* Shared fixtures                                                        */
/* ---------------------------------------------------------------------- */

/* 02:00:00:12:34:56 style MAC used everywhere. */
void ts_mac(uint8_t mac[6]);

/* Deterministic, fast configuration: short timers, all features on. */
void ts_default_cfg(dhcp_client_cfg_t *cfg);

/* Convenience: init client + host port in one call. */
void ts_setup(dhcp_client_t *cl, host_port_t *hp, uint32_t seed);

/* Same, but with a caller supplied configuration (timers, MAC, hostname). */
void ts_setup_with(dhcp_client_t *cl, host_port_t *hp, uint32_t seed,
                   const dhcp_client_cfg_t *cfg);

/** Deliver a datagram to the client and return dhcp_client_input()'s result. */
int ts_deliver(dhcp_client_t *cl, const uint8_t *buf, uint16_t len);

/* Test suite entry points (implemented in test_packet.c / test_client.c /
 * test_policy.c, called from main.c). */
void test_packet_run_all(void);
void test_client_run_all(void);
void test_policy_run_all(void);

/** Build a reply from `s` and deliver it. */
int ts_deliver_reply(dhcp_client_t *cl, const reply_spec_t *s);

/* Event recorder installed by ts_setup(). */
#define TS_MAX_EVENTS 64
typedef struct {
    dhcp_event_t events[TS_MAX_EVENTS];
    dhcp_state_t old_states[TS_MAX_EVENTS]; /* old_state passed to the cb */
    dhcp_state_t states[TS_MAX_EVENTS];     /* cl->state seen by the cb   */
    unsigned count;
} ts_events_t;

ts_events_t *ts_events(void);
unsigned ts_event_count(dhcp_event_t ev);
int ts_event_seen(dhcp_event_t ev);
unsigned ts_state_change_count(void);

/** Number of STATE_CHANGE callbacks where old_state == new state (must be 0). */
unsigned ts_state_change_inconsistent(void);

/**
 * True when every STATE_CHANGE callback saw old != new, and the recorded
 * sequence of new states matches `expected` exactly.
 */
int ts_state_sequence_matches(const dhcp_state_t *expected, unsigned n);

void ts_events_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* TEST_SUPPORT_H */
