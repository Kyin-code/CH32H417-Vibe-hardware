/*
 * dhcp_port_host.h -- host-side port implementation used by the unit tests.
 *
 * Gives the DHCP core a fully deterministic environment: a virtual clock, a
 * deterministic PRNG, a capture queue for transmitted datagrams and a tiny
 * byte array standing in for NVM.
 */
#ifndef DHCP_PORT_HOST_H
#define DHCP_PORT_HOST_H

#include "dhcp_client.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HOST_TX_QUEUE 8
#define HOST_TX_MAX_LEN 640
#define HOST_LOG_QUEUE 64
#define HOST_LOG_LEN 96
#define HOST_NVM_SIZE 128

 typedef struct {
    uint16_t len;
    uint32_t dst_ip;
    uint16_t dst_port;
    uint8_t data[HOST_TX_MAX_LEN];
} host_tx_t;

typedef struct {
    /* virtual clock */
    uint32_t now_ms;
    /* deterministic PRNG state */
    uint32_t rng;

    /* captured transmissions, oldest first */
    host_tx_t tx[HOST_TX_QUEUE];
    unsigned tx_count;   /* total ever transmitted */
    unsigned tx_head;    /* index of the next slot to overwrite */

    /* when non-zero, tx_udp() fails (simulates driver back pressure) */
    int fail_tx;

    /* NVM stand-in */
    uint8_t nvm[HOST_NVM_SIZE];
    uint16_t nvm_len;
    int nvm_written;

    /* log ring */
    char log[HOST_LOG_QUEUE][HOST_LOG_LEN];
    unsigned log_count;

    dhcp_port_t port;
} host_port_t;

/** Reset everything, seed the PRNG and wire up hp->port. */
void host_port_init(host_port_t *hp, uint32_t seed);

/** Advance the virtual clock by ms without polling the client. */
void host_port_advance(host_port_t *hp, uint32_t ms);

/**
 * Advance the virtual clock in `step` millisecond increments, calling
 * dhcp_client_poll() after each one. Use step=1..50 in tests.
 */
void host_run_for(dhcp_client_t *cl, host_port_t *hp, uint32_t total_ms, uint32_t step_ms);

/** Pointer to the most recently transmitted datagram, or NULL. */
const host_tx_t *host_last_tx(host_port_t *hp);

/** Index into the TX ring: 1 == most recent, 2 == the one before, ... */
const host_tx_t *host_tx_at(host_port_t *hp, unsigned age);

/** Decode the DHCP message type of a captured datagram (0 if unparsable). */
uint8_t host_tx_msg_type(const host_tx_t *tx);

/** Count captured datagrams of a given DHCP message type. */
unsigned host_count_type(host_port_t *hp, uint8_t msg_type);

/** Drop all captured datagrams. */
void host_clear_tx(host_port_t *hp);

#ifdef __cplusplus
}
#endif

#endif /* DHCP_PORT_HOST_H */
