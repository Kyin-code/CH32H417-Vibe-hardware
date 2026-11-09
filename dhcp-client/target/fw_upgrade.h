/*
 * fw_upgrade.h -- HTTP firmware upload ("YMODEM-style" binary transfer).
 *
 * SCOPE (deliberately small, per the task):
 *   - the browser POSTs the raw .fw bytes to /upload
 *   - the receiver streams them through a CRC32 as they arrive
 *   - the transfer is declared OK only if the final CRC32 matches the expected
 *     value AND the payload size matches
 *   - nothing is written to flash and there is no filesystem; the bytes are
 *     discarded after being counted/CRC'd
 *
 * The wire format is the raw payload, i.e. exactly what an XMODEM/YMODEM sender
 * puts inside its data blocks (no framing, no 128/1024-byte padding). So a .fw
 * that already works with the desktop YMODEM path can be POSTed unchanged.
 *
 * Two ways to supply the expected CRC / size:
 *   a) headers  X-Firmware-Size / X-Firmware-CRC32  (what the web page does)
 *   b) trailer  <payload> <crc32_le> <size_le>      (last 8 bytes)
 * fw_up_feed() keeps an 8-byte delay line, so the running CRC always covers the
 * payload only and finish() can validate the trailer without buffering.
 */
#ifndef FW_UPGRADE_H
#define FW_UPGRADE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FW_UP_OK = 0,
    FW_UP_ERR_SIZE = 1,
    FW_UP_ERR_CRC = 2,
    FW_UP_ERR_STATE = 3,
    FW_UP_ERR_TOO_BIG = 4
} fw_up_result_t;

#define FW_UP_MAX_BYTES (2u * 1024u * 1024u)
#define FW_UP_TRAILER_LEN 8u
#define FW_UP_TAIL_MAX 32u

typedef struct {
    uint32_t crc;        /* running CRC32 over bytes already released        */
    uint32_t total;      /* bytes accepted (incl. the delay line)            */
    uint32_t expect;     /* expected payload size from the header, 0 = any   */
    uint32_t expect_crc; /* expected CRC32 from the header, only if have_crc */
    uint32_t payload_len; /* final payload length (valid after finish)       */
    uint32_t payload_crc; /* final payload CRC32 (valid after finish)        */
    uint8_t have_crc;
    uint8_t active;
    uint8_t finished;
    uint8_t delay_n;
    fw_up_result_t result;
    uint8_t delay[FW_UP_TRAILER_LEN];  /* 8-byte delay line for trailer mode */
    uint8_t tail[FW_UP_TAIL_MAX];      /* last bytes, for logging            */
    uint8_t tail_n;
} fw_up_t;

/** CRC32/IEEE (same polynomial/reflection as zlib's crc32). */
uint32_t fw_up_crc32(uint32_t crc, const uint8_t *data, uint32_t len);

void fw_up_begin(fw_up_t *u, uint32_t expect_size, uint32_t expect_crc);

/** Feed a chunk. 0 = ok, negative = abort. */
int fw_up_feed(fw_up_t *u, const uint8_t *data, uint32_t len);

/** Close the transfer. trailer_expected=1 means the last 8 bytes are the
 *  <crc32_le,size_le> trailer. */
fw_up_result_t fw_up_finish(fw_up_t *u, int trailer_expected);

const char *fw_up_result_str(fw_up_result_t r);

/** The single in-flight upload slot (one HTTP client at a time). */
fw_up_t *fw_up_current(void);

/** Plain-text status for GET /status. */
void fw_up_status_text(char *out, uint32_t cap);

#ifdef __cplusplus
}
#endif

#endif /* FW_UPGRADE_H */
