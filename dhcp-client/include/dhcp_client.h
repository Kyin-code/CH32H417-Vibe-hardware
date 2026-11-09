/*
 * dhcp_client.h -- public API of the portable DHCPv4 client.
 *
 * Design notes
 * ------------
 *  - Fully static: one dhcp_client_t per interface, no malloc.
 *  - Non-blocking: the application calls dhcp_client_poll() from its main
 *    loop and forwards received UDP/68 payloads to dhcp_client_input().
 *  - All state transitions are visible through an optional event callback.
 *  - Time is supplied by the port (monotonic ms), so the whole thing runs on
 *    a desktop host with a virtual clock (see tests/).
 *
 * State machine (RFC 2131 section 4.4 plus the INIT-REBOOT path):
 *
 *      INIT --start--> SELECTING --offer--> REQUESTING --ack--> BOUND
 *        ^                 |                   |                 |
 *        |                nak                 nak            T1  |
 *        |                 v                   v                 v
 *        +------------------ (restart) <---------+          RENEWING
 *        |                                                      |
 *        |                (lease expired)                   T2  |
 *        +-------------------------------------------- REBINDING+
 *
 *      BOUND/RENEWING/REBINDING --release--> RELEASING --> INIT
 *      (lease restored from NVM after reboot) INIT-REBOOT -> BOUND on ACK
 */
#ifndef DHCP_CLIENT_H
#define DHCP_CLIENT_H

#include "dhcp_config.h"
#include "dhcp_packet.h"
#include "dhcp_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Types                                                                     */
/* ------------------------------------------------------------------------- */

typedef enum {
    DHCP_STATE_INIT = 0,     /* nothing configured, waiting to be started   */
    DHCP_STATE_SELECTING,    /* DISCOVER sent, collecting OFFERs            */
    DHCP_STATE_REQUESTING,   /* REQUEST sent after an OFFER                 */
    DHCP_STATE_BOUND,        /* lease valid, renewing at T1                 */
    DHCP_STATE_RENEWING,     /* unicast REQUEST to the leasing server       */
    DHCP_STATE_REBINDING,    /* broadcast REQUEST to any server             */
    DHCP_STATE_RELEASING,    /* RELEASE sent, shutting down                 */
    DHCP_STATE_INIT_REBOOT,  /* REQUEST for a lease restored from NVM       */
    DHCP_STATE_STOPPED       /* administratively stopped                    */
} dhcp_state_t;

typedef struct dhcp_client dhcp_client_t;

/** Lease as currently held (or being validated). */
typedef struct dhcp_lease {
    uint32_t address;      /* yiaddr                                       */
    uint32_t subnet_mask;  /* option 1, 0 if the server did not send one   */
    uint32_t router;       /* option 3, first router, 0 when absent        */
    uint32_t dns[DHCP_MAX_DNS];
    uint8_t dns_count;
    uint32_t server_id;    /* option 54                                    */
    uint32_t lease_time_s; /* option 51, DHCP_INFINITE_LEASE_S = forever   */
    uint32_t t1_s;         /* option 58 or 50% of the lease time           */
    uint32_t t2_s;         /* option 59 or 87.5% of the lease time         */
    uint8_t valid;         /* 1 once bound                                  */
    uint8_t infinite;      /* 1 when lease_time_s == 0xffffffff             */
} dhcp_lease_t;

/* Events reported to the application. */
typedef enum {
    DHCP_EVENT_STATE_CHANGE = 0,
    DHCP_EVENT_BOUND,          /* fresh lease acquired                       */
    DHCP_EVENT_RENEWED,        /* lease renewed (T1/T2 path or INIT-REBOOT)   */
    DHCP_EVENT_LEASE_EXPIRED,  /* lease ran out without a renewal            */
    DHCP_EVENT_NAK,            /* server refused our request                 */
    DHCP_EVENT_DECLINED,       /* we declined an address, restarting         */
    DHCP_EVENT_RELEASED,       /* RELEASE sent / client stopped              */
    DHCP_EVENT_FAILED          /* gave up after max retries                  */
} dhcp_event_t;

/**
 * Event callback.
 *
 * @param user       pointer given to dhcp_client_init()
 * @param event      what happened
 * @param old_state  state before the transition (STATE_CHANGE only)
 * @param cl         the client (valid for the duration of the call)
 */
typedef void (*dhcp_event_cb_t)(void *user, dhcp_event_t event, dhcp_state_t old_state,
                                const dhcp_client_t *cl);

/* Runtime configuration. Zero-initialise then fill, or use
 * dhcp_client_cfg_default(). */
typedef struct {
    uint8_t mac[6];
    char hostname[DHCP_HOSTNAME_MAX]; /* "" disables option 12 */
    uint32_t retry_base_ms;           /* 0 => DHCP_DEFAULT_RETRY_BASE_MS     */
    uint32_t retry_max_ms;            /* 0 => DHCP_DEFAULT_RETRY_MAX_MS      */
    uint32_t offer_collect_ms;        /* 0 => DHCP_DEFAULT_OFFER_COLLECT_MS  */
    uint32_t decline_wait_ms;         /* 0 => DHCP_DEFAULT_DECLINE_WAIT_MS   */
    uint32_t nak_wait_ms;             /* 0 => DHCP_DEFAULT_NAK_WAIT_MS       */
    uint8_t max_retries;              /* retransmissions per state, 0 => 6   */
    uint8_t use_broadcast_flag;       /* 1 => set the BOOTP broadcast flag   */
    uint8_t send_hostname;            /* 1 => send option 12                 */
} dhcp_client_cfg_t;

/* Counters, mainly for diagnostics/tests. */
typedef struct {
    uint32_t tx_discover;
    uint32_t tx_request;
    uint32_t tx_decline;
    uint32_t tx_release;
    uint32_t rx_offer;
    uint32_t rx_ack;
    uint32_t rx_nak;
    uint32_t rx_ignored;   /* valid datagram, but not for us / not useful   */
    uint32_t rx_malformed; /* bounds/format check failed                    */
    uint32_t retransmits;
    uint32_t tx_errors;    /* port->tx_udp() returned negative              */
} dhcp_stats_t;

/* Public state of the client. Treat as read-only. */
struct dhcp_client {
    dhcp_client_cfg_t cfg;
    dhcp_port_t port;
    dhcp_event_cb_t cb;
    void *cb_user;

    dhcp_state_t state;
    dhcp_lease_t lease;     /* current lease, valid flag set when usable   */
    dhcp_lease_t candidate; /* address offered in OFFER                    */

    uint32_t xid;
    uint32_t boot_ms;        /* when the current acquisition attempt began  */
    uint32_t state_ms;       /* when the current state was entered          */
    uint32_t deadline_ms;    /* next retransmission / decision time         */
    uint32_t offer_deadline_ms;
    uint32_t t1_ms;          /* absolute: enter RENEWING                    */
    uint32_t t2_ms;          /* absolute: enter REBINDING                   */
    uint32_t expire_ms;      /* absolute: lease is gone                     */
    uint32_t lease_ms;       /* lease duration in ms (0 when infinite)      */
    uint32_t retry_interval_ms;
    uint8_t retries;
    uint8_t offer_seen;
    uint8_t started;
    uint8_t restart_pending; /* waiting out a NAK/DECLINE hold-off      */
    uint8_t txbuf[DHCP_MAX_PAYLOAD_BYTES];
    dhcp_stats_t stats;
};

/* ------------------------------------------------------------------------- */
/* API                                                                       */
/* ------------------------------------------------------------------------- */

/** Fill cfg with the defaults (clears MAC and hostname). */
void dhcp_client_cfg_default(dhcp_client_cfg_t *cfg);

/**
 * Initialise a client. The client copies cfg and port; both may live on the
 * caller's stack afterwards. State becomes DHCP_STATE_INIT.
 *
 * @return 0 on success, negative on invalid arguments.
 */
int dhcp_client_init(dhcp_client_t *cl, const dhcp_client_cfg_t *cfg, const dhcp_port_t *port,
                     dhcp_event_cb_t cb, void *cb_user);

/**
 * Start (or restart) acquisition: DHCP_STATE_INIT -> SELECTING, a DISCOVER is
 * sent immediately. Safe to call again to force a fresh acquisition.
 *
 * @return 0 on success, negative on invalid arguments / TX failure.
 */
int dhcp_client_start(dhcp_client_t *cl);

/**
 * Try to keep a lease across a reboot: validate `lease` with a REQUEST from
 * DHCP_STATE_INIT_REBOOT. On ACK the lease is renewed, on NAK it is dropped
 * and a normal DISCOVER cycle starts.
 *
 * @return 0 on success, negative on invalid arguments.
 */
int dhcp_client_restore(dhcp_client_t *cl, const dhcp_lease_t *lease);

/**
 * Drive all timers: retransmissions, OFFER collection window, T1/T2 and lease
 * expiry. Call this from the main loop (every iteration or at least every
 * 100 ms; exact timing comes from port->now_ms()).
 */
void dhcp_client_poll(dhcp_client_t *cl);

/**
 * Feed one received DHCP payload (UDP port 68 payload) into the client.
 * `len` must be the real number of valid bytes.
 *
 * @return  1 when the datagram was accepted, 0 when ignored for protocol
 *          reasons, negative on malformed input (see stats.rx_malformed).
 */
int dhcp_client_input(dhcp_client_t *cl, const uint8_t *data, uint16_t len);

/**
 * Send a DHCPRELEASE for the current lease and go to DHCP_STATE_STOPPED.
 * Clears the persisted lease. No-op when not bound.
 */
void dhcp_client_release(dhcp_client_t *cl);

/**
 * Report that the address we just got is already in use (e.g. the link layer
 * probed it with ARP and got an answer). Sends DHCPDECLINE and restarts.
 * Only meaningful in BOUND/REQUESTING.
 */
void dhcp_client_decline(dhcp_client_t *cl);

/** Stop the client without sending RELEASE; lease stays in RAM. */
void dhcp_client_stop(dhcp_client_t *cl);

/* Accessors. */
dhcp_state_t dhcp_client_get_state(const dhcp_client_t *cl);
const dhcp_lease_t *dhcp_client_get_lease(const dhcp_client_t *cl);
const dhcp_stats_t *dhcp_client_get_stats(const dhcp_client_t *cl);

/** Milliseconds until the client next needs dhcp_client_poll(), for sleep
 *  scheduling. 0 = poll right away, DHCP_POLL_INFINITE = no pending timer. */
#define DHCP_POLL_NOW 0u
#define DHCP_POLL_INFINITE 0xFFFFFFFFu
uint32_t dhcp_client_next_timeout_ms(const dhcp_client_t *cl);

/** "INIT", "SELECTING", ... */
const char *dhcp_state_name(dhcp_state_t state);

/* ------------------------------------------------------------------------- */
/* Lease persistence helpers                                                 */
/* ------------------------------------------------------------------------- */

/* Readable/writable blob sizes; the blob is versioned and CRC protected. */
#define DHCP_LEASE_BLOB_MAGIC 0x44484350u /* "DHCP" */
#define DHCP_LEASE_BLOB_VERSION 1u
#define DHCP_LEASE_BLOB_LEN 56u

/**
 * Serialise a lease into a self-contained blob (fixed DHCP_LEASE_BLOB_LEN).
 * @return blob length, or 0 on error.
 */
uint16_t dhcp_lease_serialize(const dhcp_lease_t *lease, uint8_t *out, uint16_t cap);

/**
 * Parse a blob produced by dhcp_lease_serialize().
 * @return 0 on success, negative when the blob is short/corrupt/foreign.
 */
int dhcp_lease_deserialize(const uint8_t *blob, uint16_t len, dhcp_lease_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DHCP_CLIENT_H */
