/*
 * http_srv.c -- tiny HTTP/1.1 server + firmware upload endpoint.
 *
 * No printf, no malloc: responses are built in a small static buffer and sent
 * with WCHNET_SocketSend() in a loop that honours the in/out length semantics.
 */
#include "http_srv.h"

#include <string.h>

#include "fw_upgrade.h"
#include "uprint.h"
#include "wchnet.h"
#include "eth_driver.h" /* SocketInf[] lives here, not in wchnet.h */

/* ------------------------------------------------------------------------- */
/* Output buffer helpers                                                     */
/* ------------------------------------------------------------------------- */

#define RESP_CAP 1024u

typedef struct {
    uint8_t *buf;
    uint32_t cap;
    uint32_t pos;
} ob_t;

static void ob_str(ob_t *o, const char *s)
{
    while (*s != '\0' && o->pos < o->cap) {
        o->buf[o->pos++] = (uint8_t)*s++;
    }
}

static void ob_u32(ob_t *o, uint32_t v)
{
    char tmp[10];
    uint32_t n = 0u;
    uint32_t i;

    if (v == 0u) {
        if (o->pos < o->cap) {
            o->buf[o->pos++] = '0';
        }
        return;
    }
    while (v > 0u && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    for (i = 0; i < n && o->pos < o->cap; i++) {
        o->buf[o->pos++] = (uint8_t)tmp[n - 1u - i];
    }
}

static void ob_ip(ob_t *o, uint32_t ip)
{
    ob_u32(o, (ip >> 24) & 0xFFu);
    ob_str(o, ".");
    ob_u32(o, (ip >> 16) & 0xFFu);
    ob_str(o, ".");
    ob_u32(o, (ip >> 8) & 0xFFu);
    ob_str(o, ".");
    ob_u32(o, ip & 0xFFu);
}

/* Send everything, honouring the in/out length convention. */
static int send_all(uint8_t sockid, const uint8_t *data, uint32_t len)
{
    uint32_t sent = 0u;

    while (sent < len) {
        uint32_t chunk = len - sent;
        uint8_t rc = WCHNET_SocketSend(sockid, (uint8_t *)(uintptr_t)&data[sent], &chunk);

        if (rc != WCHNET_ERR_SUCCESS || chunk == 0u) {
            return -1;
        }
        sent += chunk;
    }
    return 0;
}

static void send_str(uint8_t sockid, const char *s)
{
    uint32_t n = 0u;

    while (s[n] != '\0') {
        n++;
    }
    if (n > 0u) {
        (void)send_all(sockid, (const uint8_t *)s, n);
    }
}

/* ------------------------------------------------------------------------- */
/* The page                                                                  */
/* ------------------------------------------------------------------------- */

static const char k_page[] =
    "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
    "Cache-Control: no-store\r\nConnection: close\r\n\r\n"
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>CH32H417 firmware upload</title><style>"
    "body{font:15px system-ui,sans-serif;max-width:44rem;margin:2rem auto;padding:0 1rem;}"
    "h1{font-size:1.3rem}code{background:#eee;padding:.1rem .3rem;border-radius:3px}"
    ".box{border:1px solid #ddd;border-radius:8px;padding:1rem;margin:1rem 0}"
    "progress{width:100%;height:1.2rem}#log{white-space:pre-wrap;font-family:ui-monospace,monospace;"
    "font-size:.85rem;background:#f7f7f7;padding:.7rem;border-radius:6px;min-height:3rem}"
    "button{font-size:1rem;padding:.5rem 1rem;border-radius:6px;border:1px solid #888;"
    "background:#fff;cursor:pointer}button:disabled{opacity:.5;cursor:default}"
    "</style></head><body>"
    "<h1>CH32H417 固件上传</h1>"
    "<div class=box><b>设备状态</b><div id=stat>读取中…</div></div>"
    "<div class=box><b>上传 .fw</b><br><br>"
    "<input type=file id=f accept='.fw,.bin'><br><br>"
    "<button id=go disabled>开始上传</button>"
    "<progress id=p value=0 max=100></progress>"
    "<div id=log>等待文件…</div></div>"
    "<script>"
    "const f=document.getElementById('f'),go=document.getElementById('go'),"
    "p=document.getElementById('p'),log=document.getElementById('log'),"
    "stat=document.getElementById('stat');"
    "function say(m){log.textContent=m}"
    "async function refresh(){try{const r=await fetch('/status',{cache:'no-store'});"
    "stat.textContent=await r.text();stat.style.whiteSpace='pre'}catch(e){stat.textContent='未连接'}}"
    "f.onchange=()=>{go.disabled=!f.files.length;if(f.files.length)say('已选择 '+f.files[0].name+'  '+f.files[0].size+' 字节（YMODEM 数据载荷）')};"
    "go.onclick=async()=>{const file=f.files[0];if(!file)return;go.disabled=true;say('计算 CRC32…');"
    "const buf=await file.arrayBuffer();"
    "/* CRC32/IEEE，和固件里的 fw_up_crc32 完全一致 */"
    "const tab=new Uint32Array(256);for(let i=0;i<256;i++){let c=i;for(let k=0;k<8;k++)c=(c&1)?(0xEDB88320^(c>>>1)):(c>>>1);tab[i]=c>>>0}"
    "let crc=0xFFFFFFFF>>>0;const u=new Uint8Array(buf);"
    "for(let i=0;i<u.length;i++)crc=(tab[(crc^u[i])&0xFF]^(crc>>>8))>>>0;"
    "crc=(crc^0xFFFFFFFF)>>>0;const crcHex=crc.toString(16).padStart(8,'0');"
    "say('CRC32='+crcHex+'\\nPOST /upload …');"
    "const xhr=new XMLHttpRequest();xhr.open('POST','/upload');"
    "xhr.setRequestHeader('X-Firmware-Size',String(file.size));"
    "xhr.setRequestHeader('X-Firmware-CRC32',crcHex);"
    "xhr.upload.onprogress=e=>{if(e.lengthComputable){const pc=Math.round(e.loaded*100/e.total);p.value=pc;"
    "say('上传中 '+pc+'%  ('+e.loaded+'/'+e.total+')')}};"
    "xhr.onload=()=>{say('HTTP '+xhr.status+'\\n'+xhr.responseText);go.disabled=false;refresh()};"
    "xhr.onerror=()=>{say('网络错误');go.disabled=false};"
    "xhr.send(file)};"
    "refresh();setInterval(refresh,2000);"
    "</script></body></html>";

/* ------------------------------------------------------------------------- */
/* Request parsing                                                           */
/* ------------------------------------------------------------------------- */

static int ci_eq(const char *a, const char *b, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
    }
    return 1;
}

static int starts_with_ci(const char *s, const char *prefix)
{
    uint32_t n = 0u;

    while (prefix[n] != '\0') {
        n++;
    }
    return ci_eq(s, prefix, n);
}

static uint32_t parse_u32(const char *s, uint32_t len, uint8_t hex)
{
    uint32_t v = 0u;
    uint32_t i;

    if (len >= 2u && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        len -= 2u;
        hex = 1u;
    }
    for (i = 0; i < len; i++) {
        char c = s[i];
        uint32_t d;

        if (c >= '0' && c <= '9') {
            d = (uint32_t)(c - '0');
        } else if (hex != 0u && c >= 'a' && c <= 'f') {
            d = (uint32_t)(c - 'a' + 10);
        } else if (hex != 0u && c >= 'A' && c <= 'F') {
            d = (uint32_t)(c - 'A' + 10);
        } else if (c == ' ' || c == '\r' || c == '\n' || c == '\t') {
            break;
        } else {
            return v;
        }
        if (hex != 0u) {
            v = (v << 4) + d;
        } else {
            v = v * 10u + d;
        }
    }
    return v;
}

static uint32_t hdr_value(const char *req, uint32_t len, const char *name, char *out,
                          uint32_t cap)
{
    const char *p = req;
    const char *end = req + len;
    uint32_t nlen = 0u;
    uint32_t i;

    while (name[nlen] != '\0') {
        nlen++;
    }

    /* skip the request line */
    while (p < end && *p != '\n') {
        p++;
    }
    while (p < end) {
        const char *line_end;
        const char *colon;

        if (*p == '\n') {
            p++;
        }
        line_end = p;
        while (line_end < end && *line_end != '\n') {
            line_end++;
        }
        if (line_end == p) {
            break;
        }
        if (ci_eq(p, name, nlen) == 0 || (uint32_t)(line_end - p) <= nlen ||
            p[nlen] != ':') {
            p = line_end;
            continue;
        }
        colon = p + nlen + 1u;
        while (colon < line_end && (*colon == ' ' || *colon == '\t')) {
            colon++;
        }
        for (i = 0; colon + i < line_end && i + 1u < cap; i++) {
            out[i] = colon[i];
        }
        out[i] = '\0';
        return i;
    }
    return 0u;
}

/* ------------------------------------------------------------------------- */
/* Responses                                                                 */
/* ------------------------------------------------------------------------- */

static void send_status(http_srv_t *s, int code, const char *reason, const char *extra_hdr,
                        const char *body)
{
    ob_t o;
    uint8_t buf[RESP_CAP];
    uint32_t blen = 0u;

    while (body[blen] != '\0') {
        blen++;
    }
    o.buf = buf;
    o.cap = RESP_CAP;
    o.pos = 0u;

    ob_str(&o, "HTTP/1.1 ");
    ob_u32(&o, (uint32_t)code);
    ob_str(&o, " ");
    ob_str(&o, reason);
    ob_str(&o, "\r\nContent-Type: text/plain; charset=utf-8\r\n"
               "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n"
               "Access-Control-Allow-Headers: X-Firmware-Size, X-Firmware-CRC32\r\n"
               "Connection: close\r\n");
    if (extra_hdr != 0) {
        ob_str(&o, extra_hdr);
    }
    ob_str(&o, "Content-Length: ");
    ob_u32(&o, blen);
    ob_str(&o, "\r\n\r\n");
    ob_str(&o, body);

    (void)send_all(s->conn_id, buf, o.pos);
}

static void send_page(http_srv_t *s)
{
    uint32_t n = 0u;

    while (k_page[n] != '\0') {
        n++;
    }
    (void)send_all(s->conn_id, (const uint8_t *)k_page, n);
}

static void send_status_text(http_srv_t *s)
{
    char body[192];

    fw_up_status_text(body, (uint32_t)sizeof(body));
    send_status(s, 200, "OK", 0, body);
}

/* ------------------------------------------------------------------------- */
/* Upload streaming                                                          */
/* ------------------------------------------------------------------------- */

static void begin_post(http_srv_t *s, const char *req)
{
    char val[16];
    uint32_t n;

    s->expect_size = 0u;
    s->expect_crc = 0u;
    s->have_crc = 0u;
    s->trailer_mode = 0u;

    n = hdr_value(req, s->header_len, "X-Firmware-Size", val, sizeof(val));
    if (n > 0u) {
        s->expect_size = parse_u32(val, n, 0u);
    }
    n = hdr_value(req, s->header_len, "X-Firmware-CRC32", val, sizeof(val));
    if (n > 0u) {
        s->expect_crc = parse_u32(val, n, 1u);
        s->have_crc = 1u;
    }
    if (s->content_length != 0u && s->expect_size != 0u &&
        s->content_length == s->expect_size + FW_UP_TRAILER_LEN) {
        s->trailer_mode = 1u;
    }

    fw_up_begin(fw_up_current(), s->expect_size, s->expect_crc);
    s->post_in_progress = 1u;
    s->post_count++;
}

static void body_chunk(http_srv_t *s, const uint8_t *data, uint32_t len)
{
    if (s->post_in_progress == 0u) {
        return; /* ignoring a body we do not understand */
    }
    if (fw_up_feed(fw_up_current(), data, len) != 0) {
        s->post_in_progress = 0u;
        s->err_count++;
        return;
    }
    s->body_seen += len;
}

static void finish_post(http_srv_t *s)
{
    fw_up_result_t r;
    char body[160];
    ob_t o;

    if (s->post_in_progress == 0u) {
        s->err_count++;
        send_status(s, 400, "Bad Request", 0, "no upload in progress\n");
        return;
    }
    r = fw_up_finish(fw_up_current(), s->trailer_mode != 0u ? 1 : 0);
    s->post_in_progress = 0u;

    o.buf = (uint8_t *)body;
    o.cap = sizeof(body);
    o.pos = 0u;
    ob_str(&o, "result=");
    ob_str(&o, fw_up_result_str(r));
    ob_str(&o, "\nbytes=");
    ob_u32(&o, s->body_seen);
    ob_str(&o, "\ncrc32=0x");
    {
        char hex[9];
        static const char h[] = "0123456789abcdef";
        uint32_t v = fw_up_current()->crc;
        uint32_t i;

        for (i = 0; i < 8u; i++) {
            hex[i] = h[(v >> (28u - 4u * i)) & 0xFu];
        }
        hex[8] = '\0';
        ob_str(&o, hex);
    }
    ob_str(&o, "\n");

    if (r == FW_UP_OK) {
        s->ok_count++;
        send_status(s, 200, "OK", 0, body);
    } else {
        s->err_count++;
        send_status(s, 400, "Bad Request", 0, body);
    }
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

int http_start(http_srv_t *s, const char *host_label)
{
    SOCK_INF inf;
    uint8_t rc;
    uint32_t i;

    if (s == 0) {
        return -1;
    }
    memset(s, 0, sizeof(*s));
    s->listen_open = 0u;
    s->conn_open = 0u;
    if (host_label != 0) {
        for (i = 0; i + 1u < sizeof(s->host_label) && host_label[i] != '\0'; i++) {
            s->host_label[i] = host_label[i];
        }
    }

    memset(&inf, 0, sizeof(inf));
    inf.ProtoType = PROTO_TYPE_TCP;
    inf.SourPort = (uint32_t)HTTP_SRV_PORT;
    /* Deliberately leave RecvStartPoint/RecvBufLen zero, exactly like the
     * vendored WebServer example: a LISTEN pcb is not given a receive buffer
     * until a connection is accepted (see http_poll). */

    rc = WCHNET_SocketCreat(&s->listen_id, &inf);
    if (rc != WCHNET_ERR_SUCCESS) {
        s->last_rc = rc;
        return -1;
    }
    rc = WCHNET_SocketListen(s->listen_id);
    if (rc != WCHNET_ERR_SUCCESS) {
        s->last_rc = rc;
        (void)WCHNET_SocketClose(s->listen_id, TCP_CLOSE_NORMAL);
        return -1;
    }
    s->listen_open = 1u;
    return 0;
}

void http_set_address(http_srv_t *s, uint32_t ip)
{
    if (s != 0) {
        s->ip = ip;
    }
}

int http_owns_socket(const http_srv_t *s, uint8_t sockid)
{
    if (s == 0) {
        return 0;
    }
    if (s->listen_open != 0u && s->listen_id == sockid) {
        return 1;
    }
    if (s->conn_open != 0u && s->conn_id == sockid) {
        return 1;
    }
    return 0;
}

int http_handles_socket(const http_srv_t *s, uint8_t sockid)
{
    if (s == 0 || s->listen_open == 0u) {
        return 0;
    }
    /* The listen socket AND every connection it accepts carry the server port
     * in SocketInf[].SourPort (the stack copies it on accept), so this catches
     * a brand-new connection before http_poll() has had a chance to record it.
     * The vendored WebServer example keys off the same field. */
    return SocketInf[sockid].SourPort == (uint32_t)HTTP_SRV_PORT ? 1 : 0;
}

static void close_conn(http_srv_t *s)
{
    if (s->conn_open == 0u) {
        return;
    }
    (void)WCHNET_SocketClose(s->conn_id, TCP_CLOSE_NORMAL);
    s->conn_open = 0u;
    s->conn_id = 0u;
    s->rx_len = 0u;
    s->headers_done = 0u;
    s->header_len = 0u;
    s->content_length = 0u;
    s->body_seen = 0u;
    s->post_in_progress = 0u;
    s->trailer_mode = 0u;
}

void http_poll(http_srv_t *s)
{
    uint8_t i;
    uint8_t st;

    if (s == 0) {
        return;
    }

    /* --- pick up a new connection ---------------------------------------
     * WCHNET keeps the listening socket in LISTEN state and allocates one of
     * the WCHNET_NUM_TCP sockets for each accepted connection, raising
     * SINT_STAT_CONNECT on that new socket id. The vendor WebServer example
     * only calls WCHNET_ModifyRecvBuf() on it and never re-arms the listener,
     * so we scan for whichever socket has the CONNECT bit. */
    if (s->conn_open == 0u && s->listen_open != 0u) {
        for (i = 0; i < WCHNET_MAX_SOCKET_NUM; i++) {
            SOCK_INF *inf;

            if (i == s->listen_id) {
                continue;
            }
            st = WCHNET_GetSocketInt(i);
            if ((st & SINT_STAT_CONNECT) == 0u) {
                continue;
            }
            inf = &SocketInf[i];
            s->conn_id = i;
            s->conn_open = 1u;
            s->rx_len = 0u;
            s->headers_done = 0u;
            s->header_len = 0u;
            s->content_length = 0u;
            s->body_seen = 0u;
            s->post_in_progress = 0u;
            s->trailer_mode = 0u;
            s->req_count++;
            /* Always install OUR buffer on the accepted connection; the
             * inherited one may point at the listen pcb's storage. */
            (void)inf;
            WCHNET_ModifyRecvBuf(i, (uint32_t)(uintptr_t)(void *)s->sock_rx,
                                 (uint32_t)sizeof(s->sock_rx));
            break;
        }
    }

    if (s->conn_open == 0u) {
        return;
    }

    /* --- connection socket: read and act -------------------------------- */
    st = WCHNET_GetSocketInt(s->conn_id);
    if ((st & (SINT_STAT_DISCONNECT | SINT_STAT_TIM_OUT)) != 0u) {
        close_conn(s);
        return;
    }
    if ((st & SINT_STAT_RECV) != 0u) {
        for (;;) {
            uint32_t avail = WCHNET_SocketRecvLen(s->conn_id, 0);
            uint32_t want;
            uint32_t got;

            if (avail == 0u) {
                break;
            }
            if (s->rx_len >= (uint16_t)sizeof(s->rx)) {
                /* header too large for our buffer: reject */
                send_status(s, 413, "Too Large", 0, "request too large\n");
                close_conn(s);
                return;
            }
            want = (uint32_t)sizeof(s->rx) - s->rx_len;
            if (want > avail) {
                want = avail;
            }
            got = want;
            if (WCHNET_SocketRecv(s->conn_id, &s->rx[s->rx_len], &got) !=
                WCHNET_ERR_SUCCESS) {
                break;
            }
            s->rx_len = (uint16_t)(s->rx_len + got);

            /* Once the header block is complete, decide what to do. */
            if (s->headers_done == 0u) {
                uint16_t k;
                for (k = (uint16_t)(s->rx_len > (uint16_t)got ? s->rx_len - (uint16_t)got : 0u);
                     k + 3u < s->rx_len; k++) {
                    if (s->rx[k] == '\r' && s->rx[k + 1u] == '\n' && s->rx[k + 2u] == '\r' &&
                        s->rx[k + 3u] == '\n') {
                        s->header_len = (uint16_t)(k + 4u);
                        s->headers_done = 1u;
                        break;
                    }
                }
            }
            if (s->headers_done != 0u) {
                char val[16];
                uint32_t n;
                const char *req = (const char *)s->rx;

                if (s->content_length == 0u && s->post_in_progress == 0u) {
                    n = hdr_value(req, s->header_len, "Content-Length", val, sizeof(val));
                    if (n > 0u) {
                        s->content_length = parse_u32(val, n, 0u);
                    }
                }

                /* Route on the request line. */
                if (starts_with_ci(req, "GET /status") != 0) {
                    send_status_text(s);
                    close_conn(s);
                    return;
                }
                if (starts_with_ci(req, "GET /") != 0) {
                    send_page(s);
                    close_conn(s);
                    return;
                }
                if (starts_with_ci(req, "OPTIONS") != 0) {
                    send_status(s, 204, "No Content", 0, "");
                    close_conn(s);
                    return;
                }
                if (starts_with_ci(req, "POST /upload") != 0) {
                    if (s->post_in_progress == 0u) {
                        begin_post(s, req);
                    }
                    /* Consume whatever body bytes already arrived. */
                    if (s->rx_len > s->header_len) {
                        body_chunk(s, &s->rx[s->header_len],
                                   (uint32_t)(s->rx_len - s->header_len));
                        s->rx_len = s->header_len;
                    }
                    /* Content-Length 0 => nothing else will come. */
                    if (s->content_length == 0u) {
                        finish_post(s);
                        close_conn(s);
                        return;
                    }
                } else {
                    send_status(s, 404, "Not Found", 0, "not found\n");
                    close_conn(s);
                    return;
                }
            }
        }
    }

    /* --- upload body streaming ------------------------------------------ */
    if (s->post_in_progress != 0u) {
        uint8_t chunk[256];

        for (;;) {
            uint32_t avail = WCHNET_SocketRecvLen(s->conn_id, 0);
            uint32_t want;

            if (avail == 0u) {
                break;
            }
            want = avail > sizeof(chunk) ? (uint32_t)sizeof(chunk) : avail;
            if (WCHNET_SocketRecv(s->conn_id, chunk, &want) != WCHNET_ERR_SUCCESS) {
                break;
            }
            body_chunk(s, chunk, want);
            if (s->post_in_progress == 0u) {
                break;
            }
        }
        if (s->post_in_progress != 0u && s->content_length != 0u &&
            s->body_seen >= s->content_length) {
            finish_post(s);
            close_conn(s);
        }
    }
}
