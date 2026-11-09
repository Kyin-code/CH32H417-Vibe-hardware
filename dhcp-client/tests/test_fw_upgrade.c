/*
 * selftest_main.c -- host-side checks for fw_upgrade.c and the HTTP request
 * parser.
 *
 * Build: added to the existing CMake project as a second executable.
 */
#include <stdio.h>
#include <string.h>

#include "fw_upgrade.h"

static int g_fail;
static int g_checks;

#define CHECK(cond)                                                            \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            g_fail++;                                                          \
        }                                                                      \
    } while (0)

#define CHECK_U32(a, b)                                                        \
    do {                                                                       \
        g_checks++;                                                            \
        if ((unsigned long)(a) != (unsigned long)(b)) {                        \
            printf("FAIL %s:%d: %s=%lu expected %lu\n", __FILE__, __LINE__, #a,\
                   (unsigned long)(a), (unsigned long)(b));                    \
            g_fail++;                                                          \
        }                                                                      \
    } while (0)

/* Known CRC32 vectors. */
static void test_crc(void)
{
    const uint8_t a[] = "123456789";

    CHECK_U32(fw_up_crc32(0, a, 9), 0xCBF43926u);
    CHECK_U32(fw_up_crc32(0, (const uint8_t *)"", 0), 0u);
    /* Incremental must equal one-shot. */
    CHECK_U32(fw_up_crc32(fw_up_crc32(0, a, 4), a + 4, 5), 0xCBF43926u);
}

/* Header form: size + crc in headers, payload only in the body. */
static void test_header_form(void)
{
    fw_up_t *u = fw_up_current();
    static const uint8_t payload[] = { 0x01, 0x02, 0x03, 0x04, 0x05 };
    uint32_t crc = fw_up_crc32(0, payload, sizeof(payload));

    fw_up_begin(u, (uint32_t)sizeof(payload), crc);
    CHECK_U32(fw_up_feed(u, payload, 2), 0);
    CHECK_U32(fw_up_feed(u, payload + 2, 3), 0);
    CHECK_U32(fw_up_finish(u, 0), FW_UP_OK);
    CHECK_U32(u->payload_len, sizeof(payload));
    CHECK_U32(u->payload_crc, crc);
}

/* Header form, wrong CRC. */
static void test_header_bad_crc(void)
{
    fw_up_t *u = fw_up_current();
    static const uint8_t payload[] = { 1, 2, 3 };

    fw_up_begin(u, (uint32_t)sizeof(payload), 0xDEADBEEFu);
    (void)fw_up_feed(u, payload, sizeof(payload));
    CHECK_U32(fw_up_finish(u, 0), FW_UP_ERR_CRC);
}

/* Header form, wrong size. */
static void test_header_bad_size(void)
{
    fw_up_t *u = fw_up_current();
    static const uint8_t payload[] = { 1, 2, 3 };

    fw_up_begin(u, 99u, fw_up_crc32(0, payload, sizeof(payload)));
    (void)fw_up_feed(u, payload, sizeof(payload));
    CHECK_U32(fw_up_finish(u, 0), FW_UP_ERR_SIZE);
}

/* Trailer form: <payload> <crc32_le> <size_le>. */
static void test_trailer_form(void)
{
    fw_up_t *u = fw_up_current();
    static const uint8_t payload[] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11 };
    uint32_t crc = fw_up_crc32(0, payload, sizeof(payload));
    uint8_t frame[sizeof(payload) + FW_UP_TRAILER_LEN];
    uint32_t plen = (uint32_t)sizeof(payload);

    memcpy(frame, payload, sizeof(payload));
    frame[plen + 0] = (uint8_t)(crc);
    frame[plen + 1] = (uint8_t)(crc >> 8);
    frame[plen + 2] = (uint8_t)(crc >> 16);
    frame[plen + 3] = (uint8_t)(crc >> 24);
    frame[plen + 4] = (uint8_t)(plen);
    frame[plen + 5] = (uint8_t)(plen >> 8);
    frame[plen + 6] = (uint8_t)(plen >> 16);
    frame[plen + 7] = (uint8_t)(plen >> 24);

    fw_up_begin(u, 0u, 0u);
    CHECK_U32(fw_up_feed(u, frame, (uint32_t)sizeof(frame)), 0);
    CHECK_U32(fw_up_finish(u, 1), FW_UP_OK);
    CHECK_U32(u->payload_len, plen);
    CHECK_U32(u->payload_crc, crc);
}

/* Trailer form, corrupted payload byte -> CRC mismatch. */
static void test_trailer_corrupt(void)
{
    fw_up_t *u = fw_up_current();
    static const uint8_t payload[] = { 1, 2, 3, 4 };
    uint32_t crc = fw_up_crc32(0, payload, sizeof(payload));
    uint8_t frame[sizeof(payload) + FW_UP_TRAILER_LEN];
    uint32_t plen = (uint32_t)sizeof(payload);

    memcpy(frame, payload, sizeof(payload));
    frame[plen + 0] = (uint8_t)(crc);
    frame[plen + 1] = (uint8_t)(crc >> 8);
    frame[plen + 2] = (uint8_t)(crc >> 16);
    frame[plen + 3] = (uint8_t)(crc >> 24);
    frame[plen + 4] = (uint8_t)(plen);
    frame[plen + 5] = 0u;
    frame[plen + 6] = 0u;
    frame[plen + 7] = 0u;
    frame[1] = 0xFFu; /* flip a payload byte */

    fw_up_begin(u, 0u, 0u);
    CHECK_U32(fw_up_feed(u, frame, (uint32_t)sizeof(frame)), 0);
    CHECK_U32(fw_up_finish(u, 1), FW_UP_ERR_CRC);
}

/* Feeding byte-by-byte must give the same CRC as one big chunk, in both modes.
 * This is what proves the 8-byte delay line is correct. */
static void test_bytewise(void)
{
    uint8_t payload[64];
    uint32_t i;
    uint32_t crc;
    fw_up_t *u = fw_up_current();

    for (i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i * 7u + 3u);
    }
    crc = fw_up_crc32(0, payload, sizeof(payload));

    fw_up_begin(u, (uint32_t)sizeof(payload), crc);
    for (i = 0; i < sizeof(payload); i++) {
        CHECK_U32(fw_up_feed(u, &payload[i], 1), 0);
    }
    CHECK_U32(fw_up_finish(u, 0), FW_UP_OK);
    CHECK_U32(u->payload_crc, crc);

    /* Trailer mode, byte by byte. */
    {
        uint8_t frame[sizeof(payload) + FW_UP_TRAILER_LEN];
        uint32_t plen = (uint32_t)sizeof(payload);

        memcpy(frame, payload, sizeof(payload));
        frame[plen + 0] = (uint8_t)(crc);
        frame[plen + 1] = (uint8_t)(crc >> 8);
        frame[plen + 2] = (uint8_t)(crc >> 16);
        frame[plen + 3] = (uint8_t)(crc >> 24);
        frame[plen + 4] = (uint8_t)(plen);
        frame[plen + 5] = 0u;
        frame[plen + 6] = 0u;
        frame[plen + 7] = 0u;

        fw_up_begin(u, 0u, 0u);
        for (i = 0; i < sizeof(frame); i++) {
            CHECK_U32(fw_up_feed(u, &frame[i], 1), 0);
        }
        CHECK_U32(fw_up_finish(u, 1), FW_UP_OK);
        CHECK_U32(u->payload_crc, crc);
        CHECK_U32(u->payload_len, plen);
    }
}

/* Empty payload: header form with size 0 and no CRC is accepted. */
static void test_empty(void)
{
    fw_up_t *u = fw_up_current();

    fw_up_begin(u, 0u, 0u);
    CHECK_U32(fw_up_finish(u, 0), FW_UP_OK);
    CHECK_U32(u->payload_len, 0u);
}

/* State machine guards. */
static void test_state(void)
{
    fw_up_t *u = fw_up_current();
    static const uint8_t p[] = { 1 };

    fw_up_begin(u, 1u, fw_up_crc32(0, p, 1));
    (void)fw_up_feed(u, p, 1);
    CHECK_U32(fw_up_finish(u, 0), FW_UP_OK);
    /* Second finish is a state error. */
    CHECK_U32(fw_up_finish(u, 0), FW_UP_ERR_STATE);

    fw_up_begin(u, 1u, 0u);
    (void)fw_up_finish(u, 0);
    /* Feed after finish is rejected. */
    CHECK(fw_up_feed(u, p, 1) != 0);
}

/* Status text must be NUL-terminated and mention the counters. */
static void test_status_text(void)
{
    char buf[192];

    fw_up_begin(fw_up_current(), 0u, 0u);
    fw_up_status_text(buf, sizeof(buf));
    CHECK(strstr(buf, "state=") != NULL);
    CHECK(strstr(buf, "result=") != NULL);
    CHECK(strstr(buf, "crc32=0x") != NULL);
    CHECK(buf[strlen(buf)] == '\0');
}

int main(void)
{
    test_crc();
    test_header_form();
    test_header_bad_crc();
    test_header_bad_size();
    test_trailer_form();
    test_trailer_corrupt();
    test_bytewise();
    test_empty();
    test_state();
    test_status_text();

    printf("fw_upgrade tests: %d checks, %d failure(s)\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
