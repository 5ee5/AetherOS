#include "net/dns.h"
#include "net/eth.h"
#include "net/ipv4.h"
#include "net/nic.h"
#include "net/udp.h"

#include <stdbool.h>
#include <stdint.h>

#include "lib/string.h"

#define DNS_PORT 53U

typedef struct {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} __attribute__((packed)) dns_hdr_t;

#define DNS_QR_RESPONSE 0x8000U
#define DNS_OPCODE_QUERY 0U
#define DNS_RD          0x0100U
#define DNS_TYPE_A      1U
#define DNS_CLASS_IN    1U

static volatile bool s_got_reply;
static uint32_t      s_resolved_ip;
static uint16_t      s_query_id;

static void dns_recv_cb(uint32_t src_ip, uint16_t src_port,
                         const void *data, uint16_t len, void *ctx)
{
    (void)src_ip; (void)src_port; (void)ctx;
    if (len < sizeof(dns_hdr_t)) return;
    const dns_hdr_t *hdr = (const dns_hdr_t *)data;
    if (ntohs(hdr->id) != s_query_id) return;
    if (!(ntohs(hdr->flags) & DNS_QR_RESPONSE)) return;
    if (ntohs(hdr->ancount) == 0) return;

    /* Skip over the question section. */
    const uint8_t *p   = (const uint8_t *)data + sizeof(dns_hdr_t);
    const uint8_t *end = (const uint8_t *)data + len;

    /* Skip QNAME labels. */
    while (p < end && *p != 0) {
        if ((*p & 0xc0) == 0xc0) { p += 2; goto skip_qname_done; }
        p += 1 + *p;
    }
    if (p < end) p++; /* null label */
skip_qname_done:
    p += 4; /* skip QTYPE + QCLASS */

    /* Parse answer records. */
    for (uint16_t i = 0; i < ntohs(hdr->ancount) && p < end; ++i) {
        /* Skip name (may be pointer). */
        if (p < end && (*p & 0xc0) == 0xc0) { p += 2; }
        else {
            while (p < end && *p) { p += 1 + *p; }
            if (p < end) p++;
        }
        if (end - p < 10) break;
        uint16_t rtype  = (uint16_t)((p[0] << 8) | p[1]);
        uint16_t rdlen  = (uint16_t)((p[8] << 8) | p[9]);
        p += 10;
        if (rtype == DNS_TYPE_A && rdlen == 4 && end - p >= 4) {
            memcpy(&s_resolved_ip, p, 4);
            s_got_reply = true;
            break;
        }
        p += rdlen;
    }
}

bool dns_resolve(const char *hostname, uint32_t *ip_out)
{
    uint32_t dns_ip = ipv4_dns();
    if (!dns_ip) return false;

    s_query_id  = 0x1234U;
    s_got_reply = false;

    /* Build DNS query. */
    static uint8_t qbuf[512];
    uint8_t *p = qbuf;
    dns_hdr_t *hdr = (dns_hdr_t *)p;
    p += sizeof(dns_hdr_t);
    hdr->id      = htons(s_query_id);
    hdr->flags   = htons(DNS_RD);
    hdr->qdcount = htons(1);
    hdr->ancount = 0;
    hdr->nscount = 0;
    hdr->arcount = 0;

    /* Encode QNAME. */
    const char *host = hostname;
    while (*host) {
        const char *dot = host;
        while (*dot && *dot != '.') dot++;
        uint8_t lablen = (uint8_t)(dot - host);
        *p++ = lablen;
        memcpy(p, host, lablen);
        p += lablen;
        host = (*dot) ? dot + 1 : dot;
    }
    *p++ = 0; /* root */
    *p++ = 0; *p++ = DNS_TYPE_A;
    *p++ = 0; *p++ = DNS_CLASS_IN;

    uint16_t qlen = (uint16_t)(p - qbuf);
    uint16_t src_port = 53000U;

    udp_bind(src_port, dns_recv_cb, NULL);

    /* Try up to 3 times. */
    for (int attempt = 0; attempt < 3 && !s_got_reply; ++attempt) {
        udp_send(dns_ip, src_port, DNS_PORT, qbuf, qlen);

        /* Poll for reply (~1s). */
        static uint8_t pkt[1536];
        nic_t *nic = nic_get();
        for (uint32_t i = 0; i < 200000 && !s_got_reply; ++i) {
            uint16_t n = nic->recv(nic, pkt, sizeof(pkt));
            if (n) eth_recv(pkt, n);
            for (volatile int j = 0; j < 10; ++j) {}
        }
    }

    udp_unbind(src_port);
    if (s_got_reply) { *ip_out = s_resolved_ip; return true; }
    return false;
}
