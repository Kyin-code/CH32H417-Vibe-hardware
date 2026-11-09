/*
 * mdns.h -- minimal multicast DNS responder (RFC 6762 subset).
 *
 * Advertises one A record so the board is reachable as
 *     http://ch32h417.local/
 * on the local network without knowing its DHCP address.
 *
 * Scope, deliberately small:
 *   - answers A / PTR / SRV / TXT queries for a single host name
 *   - sends one unsolicited announcement when the address becomes known
 *   - ignores everything else (no cache, no probing, no conflict resolution)
 */
#ifndef MDNS_H
#define MDNS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDNS_PORT 5353u
#define MDNS_GROUP_0 224u
#define MDNS_GROUP_1 0u
#define MDNS_GROUP_2 0u
#define MDNS_GROUP_3 251u

#ifndef MDNS_HOST_LABEL
#define MDNS_HOST_LABEL "ch32h417"
#endif

#ifndef MDNS_INSTANCE
#define MDNS_INSTANCE "CH32H417 DHCP client"
#endif

typedef struct {
    uint8_t socket_id;
    uint8_t socket_open;
    uint8_t announced;
    uint32_t ip; /* host order (wire order preserved), 0 until the lease */
    uint32_t last_rc; /* error code from the last failed WCHNET call */
    uint8_t rx_buf[512];
} mdns_t;

/** Create the multicast UDP socket on port 5353. Returns 0 on success. */
int mdns_start(mdns_t *m);

/** Tell mDNS which address to advertise; announces once on change. */
void mdns_set_address(mdns_t *m, uint32_t ip);

/** Drain and answer pending queries. Call from the main loop. */
void mdns_poll(mdns_t *m);

/** Close the socket. */
void mdns_stop(mdns_t *m);

#ifdef __cplusplus
}
#endif

#endif /* MDNS_H */
