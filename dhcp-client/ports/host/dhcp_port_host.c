#include "dhcp_port_host.h"

#include <string.h>

static uint32_t host_now_ms(void *ctx)
{
    host_port_t *hp = (host_port_t *)ctx;
    return hp->now_ms;
}

static uint32_t host_rand32(void *ctx)
{
    host_port_t *hp = (host_port_t *)ctx;
    uint32_t x = hp->rng;

    /* xorshift32: deterministic and good enough for xid/jitter tests. */
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    hp->rng = x;
    return x;
}

static int host_tx_udp(void *ctx, const uint8_t *data, uint16_t len, uint32_t dst_ip,
                       uint16_t dst_port)
{
    host_port_t *hp = (host_port_t *)ctx;
    host_tx_t *slot;

    hp->tx_count++;
    if (hp->fail_tx != 0) {
        return -1;
    }
    if (len > HOST_TX_MAX_LEN) {
        len = HOST_TX_MAX_LEN;
    }
    slot = &hp->tx[hp->tx_head];
    slot->len = len;
    slot->dst_ip = dst_ip;
    slot->dst_port = dst_port;
    memset(slot->data, 0, sizeof(slot->data));
    memcpy(slot->data, data, len);
    hp->tx_head = (hp->tx_head + 1u) % HOST_TX_QUEUE;
    return 0;
}

static void host_log(void *ctx, dhcp_log_level_t level, const char *msg)
{
    host_port_t *hp = (host_port_t *)ctx;
    unsigned i;

    (void)level;
    if (hp->log_count >= HOST_LOG_QUEUE) {
        /* keep the newest entries: shift left by one */
        for (i = 1; i < HOST_LOG_QUEUE; i++) {
            memcpy(hp->log[i - 1], hp->log[i], HOST_LOG_LEN);
        }
        hp->log_count = HOST_LOG_QUEUE - 1u;
    }
    if (msg != 0) {
        strncpy(hp->log[hp->log_count], msg, HOST_LOG_LEN - 1u);
        hp->log[hp->log_count][HOST_LOG_LEN - 1u] = '\0';
    } else {
        hp->log[hp->log_count][0] = '\0';
    }
    hp->log_count++;
}

static int host_lease_save(void *ctx, const uint8_t *blob, uint16_t len)
{
    host_port_t *hp = (host_port_t *)ctx;

    if (len > HOST_NVM_SIZE) {
        return -1;
    }
    memcpy(hp->nvm, blob, len);
    hp->nvm_len = len;
    hp->nvm_written = 1;
    return 0;
}

static int host_lease_load(void *ctx, uint8_t *blob, uint16_t cap, uint16_t *len)
{
    host_port_t *hp = (host_port_t *)ctx;

    if (hp->nvm_written == 0 || hp->nvm_len == 0u || hp->nvm_len > cap) {
        return -1;
    }
    memcpy(blob, hp->nvm, hp->nvm_len);
    *len = hp->nvm_len;
    return 0;
}

static int host_lease_clear(void *ctx)
{
    host_port_t *hp = (host_port_t *)ctx;

    hp->nvm_written = 0;
    hp->nvm_len = 0u;
    memset(hp->nvm, 0, sizeof(hp->nvm));
    return 0;
}

void host_port_init(host_port_t *hp, uint32_t seed)
{
    memset(hp, 0, sizeof(*hp));
    hp->now_ms = 1000u; /* deliberately not zero: deadline maths must not care */
    hp->rng = seed != 0u ? seed : 0x1234567u;

    hp->port.ctx = hp;
    hp->port.now_ms = host_now_ms;
    hp->port.rand32 = host_rand32;
    hp->port.tx_udp = host_tx_udp;
    hp->port.log = host_log;
    hp->port.lease_save = host_lease_save;
    hp->port.lease_load = host_lease_load;
    hp->port.lease_clear = host_lease_clear;
}

void host_port_advance(host_port_t *hp, uint32_t ms)
{
    hp->now_ms += ms;
}

void host_run_for(dhcp_client_t *cl, host_port_t *hp, uint32_t total_ms, uint32_t step_ms)
{
    uint32_t elapsed = 0u;

    if (step_ms == 0u) {
        step_ms = 1u;
    }
    /* Always poll once before advancing, so `total_ms == 0` still drives the
     * state machine once. */
    dhcp_client_poll(cl);
    while (elapsed < total_ms) {
        uint32_t step = total_ms - elapsed;
        if (step > step_ms) {
            step = step_ms;
        }
        hp->now_ms += step;
        elapsed += step;
        dhcp_client_poll(cl);
    }
}

const host_tx_t *host_tx_at(host_port_t *hp, unsigned age)
{
    unsigned idx;
    unsigned n = hp->tx_count < HOST_TX_QUEUE ? hp->tx_count : (unsigned)HOST_TX_QUEUE;

    if (age == 0u || age > n) {
        return 0;
    }
    idx = (hp->tx_head + HOST_TX_QUEUE - age) % HOST_TX_QUEUE;
    return &hp->tx[idx];
}

const host_tx_t *host_last_tx(host_port_t *hp)
{
    return host_tx_at(hp, 1u);
}

uint8_t host_tx_msg_type(const host_tx_t *tx)
{
    dhcp_msg_t msg;

    if (tx == 0) {
        return 0u;
    }
    if (dhcp_packet_parse(tx->data, tx->len, &msg) != DHCP_PARSE_OK) {
        return 0u;
    }
    return msg.msg_type;
}

unsigned host_count_type(host_port_t *hp, uint8_t msg_type)
{
    unsigned i;
    unsigned hits = 0u;
    unsigned n = hp->tx_count < HOST_TX_QUEUE ? hp->tx_count : (unsigned)HOST_TX_QUEUE;

    for (i = 1u; i <= n; i++) {
        if (host_tx_msg_type(host_tx_at(hp, i)) == msg_type) {
            hits++;
        }
    }
    return hits;
}

void host_clear_tx(host_port_t *hp)
{
    hp->tx_count = 0u;
    hp->tx_head = 0u;
    memset(hp->tx, 0, sizeof(hp->tx));
}
