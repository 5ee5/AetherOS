#include "net/checksum.h"
#include <stdint.h>

uint16_t ip_checksum(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += (uint32_t)(((uint16_t)p[0] << 8) | p[1]);
        p   += 2;
        len -= 2;
    }
    if (len) sum += (uint32_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xffffU) + (sum >> 16);
    return (uint16_t)(~sum);
}

uint16_t ip_pseudo_checksum(uint32_t src_ip, uint32_t dst_ip,
                             uint8_t proto, const void *segment, uint16_t seg_len)
{
    struct {
        uint32_t src;
        uint32_t dst;
        uint8_t  zero;
        uint8_t  proto;
        uint16_t len;
    } __attribute__((packed)) ph;
    ph.src   = src_ip;
    ph.dst   = dst_ip;
    ph.zero  = 0;
    ph.proto = proto;
    ph.len   = (uint16_t)((seg_len >> 8) | (seg_len << 8)); /* htons */

    uint32_t sum = 0;
    const uint8_t *p = (const uint8_t *)&ph;
    uint32_t len = sizeof(ph);
    while (len > 1) {
        sum += (uint32_t)(((uint16_t)p[0] << 8) | p[1]);
        p += 2; len -= 2;
    }
    p   = (const uint8_t *)segment;
    len = seg_len;
    while (len > 1) {
        sum += (uint32_t)(((uint16_t)p[0] << 8) | p[1]);
        p += 2; len -= 2;
    }
    if (len) sum += (uint32_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xffffU) + (sum >> 16);
    return (uint16_t)(~sum);
}
