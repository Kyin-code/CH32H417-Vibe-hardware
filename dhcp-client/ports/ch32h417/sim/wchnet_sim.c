#include "wchnet_sim.h"

#include <string.h>

typedef struct {
    int used;
    uint32_t proto;
    uint32_t sour_port;
    uint32_t des_port;
    uint8_t ip[4];
    uint8_t rx[SIM_RX_MAX];
    uint16_t rx_len;
    uint8_t rx_int;
    uint8_t rx_ready; /* set by WCHNET_ModifyRecvBuf() */
} sim_sock_t;

static sim_sock_t g_socks[SIM_MAX_SOCKETS];
static sim_stats_t g_stats;
static sim_tx_t g_tx[SIM_MAX_TX];
static unsigned g_tx_count;
static unsigned g_tx_head;
static int g_last_id = -1;
static uint32_t g_partial_send = 0u;

void wchnet_sim_set_partial_send(uint32_t accepted)
{
    g_partial_send = accepted;
}

uint32_t wchnet_sim_rx_pending(uint8_t sockid)
{
    if (sockid >= SIM_MAX_SOCKETS || g_socks[sockid].used == 0) {
        return 0u;
    }
    return g_socks[sockid].rx_len;
}

void wchnet_sim_reset(void)
{
    memset(g_socks, 0, sizeof(g_socks));
    memset(&g_stats, 0, sizeof(g_stats));
    memset(g_tx, 0, sizeof(g_tx));
    g_tx_count = 0u;
    g_tx_head = 0u;
    g_last_id = -1;
    g_partial_send = 0u;
}

const sim_stats_t *wchnet_sim_stats(void)
{
    return &g_stats;
}

int wchnet_sim_socket_id(void)
{
    return g_last_id;
}

static void record_tx(uint8_t via, const uint8_t *data, uint16_t len, const uint8_t *dst_ip,
                      uint16_t dst_port)
{
    sim_tx_t *slot = &g_tx[g_tx_head];

    if (len > SIM_TX_MAX) {
        len = SIM_TX_MAX;
    }
    slot->len = len;
    slot->via = via;
    slot->dst_port = dst_port;
    memset(slot->dst_ip, 0, sizeof(slot->dst_ip));
    if (dst_ip != 0) {
        memcpy(slot->dst_ip, dst_ip, 4u);
    }
    memset(slot->data, 0, sizeof(slot->data));
    if (data != 0) {
        memcpy(slot->data, data, len);
    }
    g_tx_head = (g_tx_head + 1u) % SIM_MAX_TX;
    g_tx_count++;
}

const sim_tx_t *wchnet_sim_tx_at(unsigned age)
{
    unsigned n = g_tx_count < SIM_MAX_TX ? g_tx_count : (unsigned)SIM_MAX_TX;
    unsigned idx;

    if (age == 0u || age > n) {
        return 0;
    }
    idx = (g_tx_head + SIM_MAX_TX - age) % SIM_MAX_TX;
    return &g_tx[idx];
}

const sim_tx_t *wchnet_sim_last_tx(void)
{
    return wchnet_sim_tx_at(1u);
}

/* ------------------------------------------------------------------------- */
/* Fake WCHNET API                                                           */
/* ------------------------------------------------------------------------- */

uint8_t WCHNET_SocketCreat(uint8_t *socketid, SOCK_INF *socinf)
{
    int i;

    if (socketid == 0 || socinf == 0) {
        return WCHNET_ERR_ARG;
    }
    g_stats.creat_calls++;
    for (i = 0; i < SIM_MAX_SOCKETS; i++) {
        if (g_socks[i].used == 0) {
            memset(&g_socks[i], 0, sizeof(g_socks[i]));
            g_socks[i].used = 1;
            g_socks[i].proto = socinf->ProtoType;
            g_socks[i].sour_port = socinf->SourPort;
            g_socks[i].des_port = socinf->DesPort;
            memcpy(g_socks[i].ip, socinf->IPAddr, 4u);
            *socketid = (uint8_t)i;
            g_last_id = i;
            g_stats.proto = socinf->ProtoType;
            g_stats.sour_port = socinf->SourPort;
            g_stats.des_port = socinf->DesPort;
            memcpy(g_stats.dst_ip, socinf->IPAddr, 4u);
            g_stats.recv_start = socinf->RecvStartPoint;
            g_stats.recv_buf_len = socinf->RecvBufLen;
            return WCHNET_ERR_SUCCESS;
        }
    }
    return WCHNET_ERR_BUSY;
}

uint8_t WCHNET_SocketClose(uint8_t socketid, uint8_t mode)
{
    (void)mode;
    g_stats.close_calls++;
    if (socketid < SIM_MAX_SOCKETS) {
        memset(&g_socks[socketid], 0, sizeof(g_socks[socketid]));
    }
    return WCHNET_ERR_SUCCESS;
}

uint8_t WCHNET_SocketUdpSendTo(uint8_t socketid, uint8_t *buf, uint32_t *slen, uint8_t *sip,
                               uint16_t port)
{
    uint32_t len;

    if (buf == 0 || slen == 0) {
        return WCHNET_ERR_ARG;
    }
    if (socketid >= SIM_MAX_SOCKETS || g_socks[socketid].used == 0) {
        return WCHNET_ERR_ARG;
    }
    g_stats.sendto_calls++;
    len = *slen;
    record_tx(1u, buf, (uint16_t)len, sip, port);
    if (g_partial_send != 0u && g_partial_send < len) {
        *slen = g_partial_send; /* driver took only part of the datagram */
        return WCHNET_ERR_SUCCESS;
    }
    *slen = len; /* everything sent */
    return WCHNET_ERR_SUCCESS;
}

uint8_t WCHNET_SocketSend(uint8_t socketid, uint8_t *buf, uint32_t *len)
{
    if (buf == 0 || len == 0) {
        return WCHNET_ERR_ARG;
    }
    if (socketid >= SIM_MAX_SOCKETS || g_socks[socketid].used == 0) {
        return WCHNET_ERR_ARG;
    }
    g_stats.send_calls++;
    record_tx(0u, buf, (uint16_t)(*len), g_socks[socketid].ip, (uint16_t)g_socks[socketid].des_port);
    if (g_partial_send != 0u && g_partial_send < *len) {
        *len = g_partial_send; /* partial write */
    }
    /* otherwise *len is already the sent length */
    return WCHNET_ERR_SUCCESS;
}

uint32_t WCHNET_SocketRecvLen(uint8_t socketid, uint32_t *bufaddr)
{
    (void)bufaddr;
    g_stats.recvlen_calls++;
    if (socketid >= SIM_MAX_SOCKETS || g_socks[socketid].used == 0) {
        return 0u;
    }
    return g_socks[socketid].rx_len;
}

uint8_t WCHNET_SocketRecv(uint8_t socketid, uint8_t *buf, uint32_t *len)
{
    uint32_t want;
    uint32_t have;

    if (buf == 0 || len == 0) {
        return WCHNET_ERR_ARG;
    }
    if (socketid >= SIM_MAX_SOCKETS || g_socks[socketid].used == 0) {
        return WCHNET_ERR_ARG;
    }
    g_stats.recv_calls++;
    want = *len;
    have = g_socks[socketid].rx_len;
    if (want > have) {
        want = have;
    }
    memcpy(buf, g_socks[socketid].rx, want);
    /* drop what was consumed */
    g_socks[socketid].rx_len = (uint16_t)(have - want);
    if (g_socks[socketid].rx_len != 0u) {
        memmove(g_socks[socketid].rx, &g_socks[socketid].rx[want], g_socks[socketid].rx_len);
    }
    *len = want;
    return WCHNET_ERR_SUCCESS;
}

uint8_t WCHNET_GetSocketInt(uint8_t socketid)
{
    uint8_t v = 0u;

    g_stats.getint_calls++;
    if (socketid < SIM_MAX_SOCKETS && g_socks[socketid].used != 0) {
        v = g_socks[socketid].rx_int;
        g_socks[socketid].rx_int = 0u; /* read-and-clear, like the real stack */
    }
    g_stats.last_getint_ret = v;
    return v;
}

void WCHNET_ModifyRecvBuf(uint8_t socketid, uint32_t bufaddr, uint32_t bufsize)
{
    g_stats.modifyrecvbuf_calls++;
    g_stats.modify_recv_addr = bufaddr;
    g_stats.modify_recv_len = bufsize;
    if (socketid < SIM_MAX_SOCKETS && g_socks[socketid].used != 0) {
        g_socks[socketid].rx_ready = 1u;
    }
}

int wchnet_sim_inject(uint8_t sockid, const uint8_t *data, uint16_t len)
{
    if (data == 0 || sockid >= SIM_MAX_SOCKETS || g_socks[sockid].used == 0) {
        return -1;
    }
    if (len > SIM_RX_MAX) {
        len = SIM_RX_MAX;
    }
    memcpy(g_socks[sockid].rx, data, len);
    g_socks[sockid].rx_len = len;
    g_socks[sockid].rx_int |= SINT_STAT_RECV;
    return 0;
}
