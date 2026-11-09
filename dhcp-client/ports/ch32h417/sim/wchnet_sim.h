/*
 * wchnet_sim.h -- fake WCHNET stack used to *execute* the CH32H417 port on a
 * host machine.
 *
 * The real vendor header (nanoCH32H417-main/doc/EVT/EXAM/ETH/NetLib/wchnet.h)
 * is used unchanged for types and constants; this file implements the handful
 * of WCHNET_* functions the port calls, so ports/ch32h417/dhcp_port_wchnet.c
 * can be compiled and run without a board or the RISC-V toolchain.
 *
 * What this proves: socket lifecycle, option/SOCK_INF field usage, UDP/IP_RAW
 * framing, L4 header stripping, target-IP changes and interrupt clearing.
 * What it cannot prove: that libwchnet.a behaves the same way (that needs the
 * board, see README section 12).
 */
#ifndef WCHNET_SIM_H
#define WCHNET_SIM_H

#include "wchnet.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SIM_MAX_SOCKETS 4
#define SIM_RX_MAX 640
#define SIM_TX_MAX 640
#define SIM_MAX_TX 16

typedef struct {
    uint16_t len;
    uint8_t via; /* 0 = WCHNET_SocketSend (IP_RAW), 1 = SocketUdpSendTo */
    uint8_t dst_ip[4];
    uint16_t dst_port;
    uint8_t data[SIM_TX_MAX];
} sim_tx_t;

typedef struct {
    unsigned creat_calls;
    unsigned close_calls;
    unsigned send_calls;   /* WCHNET_SocketSend */
    unsigned sendto_calls; /* WCHNET_SocketUdpSendTo */
    unsigned recvlen_calls;
    unsigned recv_calls;
    unsigned getint_calls;
    unsigned modifyrecvbuf_calls;
    uint32_t modify_recv_addr;
    uint32_t modify_recv_len;
    unsigned last_getint_ret;
    /* last created socket, as seen by the fake stack */
    uint32_t proto;
    uint32_t sour_port;
    uint32_t des_port;
    uint8_t dst_ip[4];
    uint32_t recv_start;
    uint32_t recv_buf_len;
} sim_stats_t;

void wchnet_sim_reset(void);
const sim_stats_t *wchnet_sim_stats(void);

/** Most recently transmitted datagram, or NULL. */
const sim_tx_t *wchnet_sim_last_tx(void);

/** nth most recent datagram (1 = newest). */
const sim_tx_t *wchnet_sim_tx_at(unsigned age);

/** Push a datagram into socket `sockid`'s receive queue (sets RECV int). */
int wchnet_sim_inject(uint8_t sockid, const uint8_t *data, uint16_t len);

/**
 * Make the fake stack accept only `accepted` bytes per send (0 = accept
 * everything). Used to test partial-send handling.
 */
void wchnet_sim_set_partial_send(uint32_t accepted);

/** Bytes still queued in socket `sockid`'s receive buffer. */
uint32_t wchnet_sim_rx_pending(uint8_t sockid);

/** Current socket id handed out by the fake stack, or -1. */
int wchnet_sim_socket_id(void);

#ifdef __cplusplus
}
#endif

#endif /* WCHNET_SIM_H */
