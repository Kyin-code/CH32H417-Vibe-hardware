/*
 * fw_upgrade.c -- CRC-checked streaming firmware upload (no filesystem).
 *
 * The CRC is computed over the payload only. To support the trailer form
 * (<payload> <crc32_le> <size_le>) without buffering the whole image, the feed
 * path keeps an 8-byte delay line: bytes leave the delay line and enter the CRC
 * only once 8 newer bytes have arrived. At finish() the delay line holds exactly
 * the trailer, so the running CRC covers the payload alone.
 */
#include "fw_upgrade.h"

#include <string.h>

static fw_up_t g_up;

uint32_t fw_up_crc32(uint32_t crc, const uint8_t *data, uint32_t len)
{
    uint32_t c = ~crc;
    uint32_t i;
    uint8_t b;

    for (i = 0; i < len; i++) {
        c ^= (uint32_t)data[i];
        for (b = 0; b < 8u; b++) {
            if ((c & 1u) != 0u) {
                c = (c >> 1) ^ 0xEDB88320u;
            } else {
                c >>= 1;
            }
        }
    }
    return ~c;
}

fw_up_t *fw_up_current(void)
{
    return &g_up;
}

void fw_up_begin(fw_up_t *u, uint32_t expect_size, uint32_t expect_crc)
{
    if (u == 0) {
        return;
    }
    memset(u, 0, sizeof(*u));
    u->crc = 0u;
    u->expect = expect_size;
    u->expect_crc = expect_crc;
    u->have_crc = (expect_crc != 0u) ? 1u : 0u;
    u->active = 1u;
    u->result = FW_UP_OK;
}

/* Push one byte through the 8-byte delay line into the CRC. */
static void delay_push(fw_up_t *u, uint8_t b)
{
    if (u->delay_n == FW_UP_TRAILER_LEN) {
        u->crc = fw_up_crc32(u->crc, &u->delay[0], 1u);
        memmove(&u->delay[0], &u->delay[1], FW_UP_TRAILER_LEN - 1u);
        u->delay[FW_UP_TRAILER_LEN - 1u] = b;
    } else {
        u->delay[u->delay_n++] = b;
    }
}

int fw_up_feed(fw_up_t *u, const uint8_t *data, uint32_t len)
{
    uint32_t i;

    if (u == 0 || u->active == 0u || u->finished != 0u) {
        return -1;
    }
    if (data == 0 || len == 0u) {
        return 0;
    }
    if ((uint64_t)u->total + (uint64_t)len > (uint64_t)FW_UP_MAX_BYTES) {
        u->result = FW_UP_ERR_TOO_BIG;
        u->active = 0u;
        return -2;
    }
    for (i = 0; i < len; i++) {
        delay_push(u, data[i]);
    }
    u->total += len;
    return 0;
}

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

fw_up_result_t fw_up_finish(fw_up_t *u, int trailer_expected)
{
    if (u == 0 || u->active == 0u || u->finished != 0u) {
        if (u != 0) {
            u->result = FW_UP_ERR_STATE;
        }
        return FW_UP_ERR_STATE;
    }
    u->finished = 1u;
    u->active = 0u;

    if (trailer_expected != 0) {
        uint32_t trailer_crc;
        uint32_t trailer_size;

        if (u->total < FW_UP_TRAILER_LEN + 1u || u->delay_n != FW_UP_TRAILER_LEN) {
            u->result = FW_UP_ERR_SIZE;
            return u->result;
        }
        /* The delay line holds the trailer; u->crc already covers the payload
         * before it. */
        u->payload_len = u->total - FW_UP_TRAILER_LEN;
        u->payload_crc = u->crc;
        trailer_crc = rd_le32(&u->delay[0]);
        trailer_size = rd_le32(&u->delay[4]);

        if (trailer_size != u->payload_len) {
            u->result = FW_UP_ERR_SIZE;
            return u->result;
        }
        if (trailer_crc != u->payload_crc) {
            u->result = FW_UP_ERR_CRC;
            return u->result;
        }
        if (u->have_crc != 0u && u->expect_crc != u->payload_crc) {
            u->result = FW_UP_ERR_CRC;
            return u->result;
        }
        u->result = FW_UP_OK;
        return u->result;
    }

    /* No trailer: everything that arrived is payload. */
    u->payload_len = u->total;
    u->payload_crc = fw_up_crc32(u->crc, u->delay, u->delay_n);

    if (u->expect != 0u && u->payload_len != u->expect) {
        u->result = FW_UP_ERR_SIZE;
        return u->result;
    }
    if (u->have_crc != 0u && u->expect_crc != u->payload_crc) {
        u->result = FW_UP_ERR_CRC;
        return u->result;
    }
    u->result = FW_UP_OK;
    return u->result;
}

const char *fw_up_result_str(fw_up_result_t r)
{
    switch (r) {
    case FW_UP_OK: return "ok";
    case FW_UP_ERR_SIZE: return "size-mismatch";
    case FW_UP_ERR_CRC: return "crc-mismatch";
    case FW_UP_ERR_STATE: return "bad-state";
    case FW_UP_ERR_TOO_BIG: return "too-big";
    default: return "unknown";
    }
}

/* ------------------------------------------------------------------------- */
/* Tiny formatting helpers (no printf)                                       */
/* ------------------------------------------------------------------------- */

static uint32_t u32_to_dec(uint32_t v, char *out)
{
    char tmp[10];
    uint32_t n = 0u;
    uint32_t i;

    if (v == 0u) {
        out[0] = '0';
        return 1u;
    }
    while (v > 0u && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    for (i = 0; i < n; i++) {
        out[i] = tmp[n - 1u - i];
    }
    return n;
}

static void u32_to_hex8(uint32_t v, char *out)
{
    static const char h[] = "0123456789abcdef";
    uint32_t i;

    for (i = 0; i < 8u; i++) {
        out[i] = h[(v >> (28u - 4u * i)) & 0xFu];
    }
    out[8] = '\0';
}

static uint32_t str_put(char *out, uint32_t pos, uint32_t cap, const char *s)
{
    while (*s != '\0' && pos + 1u < cap) {
        out[pos++] = *s++;
    }
    if (cap > 0u) {
        out[pos < cap ? pos : cap - 1u] = '\0';
    }
    return pos;
}

static uint32_t num_put(char *out, uint32_t pos, uint32_t cap, uint32_t v)
{
    char num[11];
    uint32_t n = u32_to_dec(v, num);
    uint32_t i;

    for (i = 0; i < n && pos + 1u < cap; i++) {
        out[pos++] = num[i];
    }
    if (cap > 0u) {
        out[pos < cap ? pos : cap - 1u] = '\0';
    }
    return pos;
}

void fw_up_status_text(char *out, uint32_t cap)
{
    uint32_t p = 0u;
    char hex[9];

    if (out == 0 || cap == 0u) {
        return;
    }
    out[0] = '\0';

    p = str_put(out, p, cap, "state=");
    p = str_put(out, p, cap, g_up.active != 0u ? "receiving" : "idle");
    p = str_put(out, p, cap, "\nresult=");
    p = str_put(out, p, cap, fw_up_result_str(g_up.result));
    p = str_put(out, p, cap, "\nreceived=");
    p = num_put(out, p, cap, g_up.total);
    p = str_put(out, p, cap, "\npayload=");
    p = num_put(out, p, cap, g_up.payload_len);
    p = str_put(out, p, cap, "\ncrc32=0x");
    u32_to_hex8(g_up.finished != 0u ? g_up.payload_crc : g_up.crc, hex);
    p = str_put(out, p, cap, hex);
    p = str_put(out, p, cap, "\n");
}
