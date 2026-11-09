/*
 * http_srv.h -- tiny HTTP/1.1 server for the firmware upload page.
 *
 * Endpoints:
 *   GET  /                 the upload page (single self-contained HTML)
 *   GET  /status           plain text: state / result / bytes / crc32
 *   POST /upload           body = raw .fw bytes, streaming CRC32
 *   OPTIONS *              CORS preflight (so a desktop tool can POST too)
 *
 * Headers understood on POST /upload:
 *   X-Firmware-Size: <decimal>    expected payload length
 *   X-Firmware-CRC32: <hex/decimal> expected CRC32 (IEEE)
 *   Content-Length:  <decimal>    if size+8, the last 8 bytes are a
 *                                 <crc32,size> little-endian trailer
 *
 * One connection is served at a time; the listening socket keeps accepting.
 */
#ifndef HTTP_SRV_H
#define HTTP_SRV_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HTTP_SRV_PORT 80u
#define HTTP_SRV_RX_BUF 1460u

typedef struct {
    uint8_t listen_id;
    uint8_t listen_open;
    uint8_t conn_id;
    uint8_t conn_open;
    uint32_t ip;              /* our address, for the page and headers */
    char host_label[32];
    /* request assembly */
    uint8_t rx[HTTP_SRV_RX_BUF];
    uint16_t rx_len;          /* bytes buffered in rx                     */
    uint8_t headers_done;
    uint16_t header_len;
    uint32_t content_length;
    uint32_t body_seen;
    uint32_t expect_size;
    uint32_t expect_crc;
    uint8_t have_crc;
    uint8_t trailer_mode;
    uint8_t post_in_progress;
    /* stats */
    uint32_t req_count;
    uint32_t post_count;
    uint32_t ok_count;
    uint32_t err_count;
    uint32_t last_rc; /* error code from the last failed WCHNET call */
    /* Our own receive buffer for the accepted connection.  The stack does not
     * necessarily hand an accepted socket a buffer of its own, and
     * WCHNET_ModifyRecvBuf(ptr, 0) would silently give the connection nowhere
     * to land - the vendored WebServer example passes its own array here too. */
    uint8_t sock_rx[HTTP_SRV_RX_BUF];
} http_srv_t;

/** Create the listening socket. Returns 0 on success. */
int http_start(http_srv_t *s, const char *host_label);

/** Tell the server our address (used in the page and in Location headers). */
void http_set_address(http_srv_t *s, uint32_t ip);

/**
 * Drive the server: accept connections, parse requests, stream uploads.
 * Call from the main loop after WCHNET_MainTask().
 */
void http_poll(http_srv_t *s);

/** True when `sockid` belongs to this server (so the global handler skips it). */
int http_owns_socket(const http_srv_t *s, uint8_t sockid);

/**
 * True when `sockid` carries our server port, including connections accepted
 * after http_start(). Use this in the global socket-interrupt sweep so a brand
 * new connection is not drained (and its CONNECT bit swallowed) by the
 * catch-all loop before http_poll() sees it.
 */
int http_handles_socket(const http_srv_t *s, uint8_t sockid);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_SRV_H */
