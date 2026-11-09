/*
 * dhcp_lease.c -- lease persistence blob (versioned + CRC32 protected).
 *
 * The blob is what a port stores in NVM / retained RAM so a node can come
 * back after a power cut and either keep using its address (validated with a
 * REQUEST from DHCP_STATE_INIT_REBOOT) instead of starting from DISCOVER.
 */
#include "dhcp_client.h"

#include <string.h>

/* Layout (little endian, 56 bytes):
 *   0  magic           4
 *   4  version         1
 *   5  flags           1   bit0 valid, bit1 infinite
 *   6  reserved        2
 *   8  address         4
 *  12  subnet_mask     4
 *  16  router          4
 *  20  server_id       4
 *  24  lease_time_s    4
 *  28  t1_s            4
 *  32  t2_s            4
 *  36  dns_count       1
 *  37  reserved        3
 *  40  dns[0]          4
 *  44  dns[1]          4
 *  48  reserved        4
 *  52  crc32           4  (over bytes 0..51)
 */
#define BLOB_SIZE 56u
#define BLOB_CRC_OFFSET 52u
#define BLOB_FLAG_VALID 0x01u
#define BLOB_FLAG_INFINITE 0x02u

static const uint32_t k_magic = DHCP_LEASE_BLOB_MAGIC;

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t crc32(const uint8_t *data, uint16_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint16_t i;
    uint8_t b;

    for (i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i];
        for (b = 0; b < 8u; b++) {
            if ((crc & 1u) != 0u) {
                crc = (crc >> 1) ^ 0xEDB88320u;
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc;
}

uint16_t dhcp_lease_serialize(const dhcp_lease_t *lease, uint8_t *out, uint16_t cap)
{
    uint8_t flags = 0u;

    if (lease == 0 || out == 0 || cap < BLOB_SIZE) {
        return 0u;
    }
    memset(out, 0, BLOB_SIZE);
    if (lease->valid != 0u) {
        flags |= BLOB_FLAG_VALID;
    }
    if (lease->infinite != 0u) {
        flags |= BLOB_FLAG_INFINITE;
    }
    put_u32(&out[0], k_magic);
    out[4] = (uint8_t)DHCP_LEASE_BLOB_VERSION;
    out[5] = flags;
    put_u32(&out[8], lease->address);
    put_u32(&out[12], lease->subnet_mask);
    put_u32(&out[16], lease->router);
    put_u32(&out[20], lease->server_id);
    put_u32(&out[24], lease->lease_time_s);
    put_u32(&out[28], lease->t1_s);
    put_u32(&out[32], lease->t2_s);
    out[36] = lease->dns_count;
    put_u32(&out[40], lease->dns[0]);
#if DHCP_MAX_DNS > 1
    put_u32(&out[44], lease->dns[1]);
#endif
    put_u32(&out[BLOB_CRC_OFFSET], crc32(out, (uint16_t)BLOB_CRC_OFFSET));

    return (uint16_t)BLOB_SIZE;
}

int dhcp_lease_deserialize(const uint8_t *blob, uint16_t len, dhcp_lease_t *out)
{
    uint8_t flags;

    if (blob == 0 || out == 0 || len < BLOB_SIZE) {
        return -1;
    }
    if (get_u32(&blob[0]) != k_magic) {
        return -2;
    }
    if (blob[4] != (uint8_t)DHCP_LEASE_BLOB_VERSION) {
        return -3;
    }
    if (get_u32(&blob[BLOB_CRC_OFFSET]) != crc32(blob, (uint16_t)BLOB_CRC_OFFSET)) {
        return -4;
    }

    flags = blob[5];
    memset(out, 0, sizeof(*out));
    out->address = get_u32(&blob[8]);
    out->subnet_mask = get_u32(&blob[12]);
    out->router = get_u32(&blob[16]);
    out->server_id = get_u32(&blob[20]);
    out->lease_time_s = get_u32(&blob[24]);
    out->t1_s = get_u32(&blob[28]);
    out->t2_s = get_u32(&blob[32]);
    out->dns_count = blob[36];
    if (out->dns_count > (uint8_t)DHCP_MAX_DNS) {
        out->dns_count = (uint8_t)DHCP_MAX_DNS;
    }
    out->dns[0] = get_u32(&blob[40]);
#if DHCP_MAX_DNS > 1
    out->dns[1] = get_u32(&blob[44]);
#endif
    out->valid = (flags & BLOB_FLAG_VALID) != 0u ? 1u : 0u;
    out->infinite = (flags & BLOB_FLAG_INFINITE) != 0u ? 1u : 0u;
    if (out->address == 0u) {
        return -5;
    }
    return 0;
}
