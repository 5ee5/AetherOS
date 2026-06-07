#include "net/udp.h"
#include "net/checksum.h"
#include "net/eth.h"
#include "net/ipv4.h"

#include <stddef.h>
#include <stdint.h>

#include "lib/string.h"

#define MAX_BINDS 16

typedef struct {
    uint16_t    port;
    udp_recv_fn fn;
    void       *ctx;
    bool        used;
} udp_bind_t;

typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed)) udp_hdr_t;

static udp_bind_t s_binds[MAX_BINDS];

void udp_bind(uint16_t port, udp_recv_fn fn, void *ctx)
{
    for (int i = 0; i < MAX_BINDS; ++i) {
        if (!s_binds[i].used) {
            s_binds[i].port = port;
            s_binds[i].fn   = fn;
            s_binds[i].ctx  = ctx;
            s_binds[i].used = true;
            return;
        }
    }
}

void udp_unbind(uint16_t port)
{
    for (int i = 0; i < MAX_BINDS; ++i)
        if (s_binds[i].used && s_binds[i].port == port)
            s_binds[i].used = false;
}

void udp_recv(uint32_t src_ip, uint32_t dst_ip, const void *pkt, uint16_t len)
{
    (void)dst_ip;
    if (len < sizeof(udp_hdr_t)) return;
    const udp_hdr_t *hdr = (const udp_hdr_t *)pkt;
    uint16_t dport = ntohs(hdr->dst_port);
    const uint8_t *data = (const uint8_t *)pkt + sizeof(udp_hdr_t);
    uint16_t dlen  = (uint16_t)(ntohs(hdr->length) - sizeof(udp_hdr_t));
    if (dlen > len - sizeof(udp_hdr_t)) return;

    for (int i = 0; i < MAX_BINDS; ++i) {
        if (s_binds[i].used && s_binds[i].port == dport)
            s_binds[i].fn(src_ip, ntohs(hdr->src_port), data, dlen, s_binds[i].ctx);
    }
}

bool udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
              const void *data, uint16_t len)
{
    static uint8_t buf[1472]; /* 1500 - 20 IP - 8 UDP */
    if (sizeof(udp_hdr_t) + len > sizeof(buf)) return false;

    udp_hdr_t *hdr = (udp_hdr_t *)buf;
    hdr->src_port = htons(src_port);
    hdr->dst_port = htons(dst_port);
    hdr->length   = htons((uint16_t)(sizeof(udp_hdr_t) + len));
    hdr->checksum = 0;
    memcpy(buf + sizeof(udp_hdr_t), data, len);

    uint16_t total = (uint16_t)(sizeof(udp_hdr_t) + len);
    uint16_t csum  = ip_pseudo_checksum(ipv4_our_ip(), dst_ip, IP_PROTO_UDP, buf, total);
    hdr->checksum  = htons(csum);

    return ipv4_send(dst_ip, IP_PROTO_UDP, buf, total);
}
