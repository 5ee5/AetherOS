#include "net/icmp.h"
#include "net/checksum.h"
#include "net/eth.h"
#include "net/ipv4.h"

#include <stdint.h>

#include "lib/string.h"

#define ICMP_ECHO_REQUEST 8U
#define ICMP_ECHO_REPLY   0U

typedef struct {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed)) icmp_hdr_t;

void icmp_recv(uint32_t src_ip, const void *pkt, uint16_t len)
{
    if (len < sizeof(icmp_hdr_t)) return;
    const icmp_hdr_t *hdr = (const icmp_hdr_t *)pkt;

    if (hdr->type == ICMP_ECHO_REQUEST && hdr->code == 0) {
        /* Send echo reply. */
        static uint8_t reply[1024];
        if (len > sizeof(reply)) return;
        memcpy(reply, pkt, len);
        icmp_hdr_t *r = (icmp_hdr_t *)reply;
        r->type     = ICMP_ECHO_REPLY;
        r->checksum = 0;
        r->checksum = htons(ip_checksum(reply, len));
        ipv4_send(src_ip, IP_PROTO_ICMP, reply, len);
    }
}
