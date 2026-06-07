#include "net/ipv4.h"
#include "net/arp.h"
#include "net/checksum.h"
#include "net/eth.h"
#include "net/icmp.h"
#include "net/udp.h"
#include "net/tcp.h"

#include <stdint.h>

#include "lib/string.h"

static uint32_t s_ip;
static uint32_t s_netmask;
static uint32_t s_gateway;
static uint32_t s_dns;
static uint16_t s_id;

uint32_t ipv4_our_ip(void)  { return s_ip; }
uint32_t ipv4_gateway(void) { return s_gateway; }
uint32_t ipv4_dns(void)     { return s_dns; }

void ipv4_set_config(uint32_t ip, uint32_t netmask, uint32_t gateway, uint32_t dns)
{
    s_ip      = ip;
    s_netmask = netmask;
    s_gateway = gateway;
    s_dns     = dns;
}

void ipv4_recv(const uint8_t src_mac[6], const void *pkt, uint16_t len)
{
    if (len < sizeof(ip_hdr_t)) return;
    const ip_hdr_t *ip = (const ip_hdr_t *)pkt;
    uint8_t ihl = (ip->ver_ihl & 0x0fU) * 4U;
    if (ihl < 20 || ihl > len) return;

    /* Cache ARP entry for sender. */
    arp_insert(ip->src, src_mac);

    const uint8_t *payload = (const uint8_t *)pkt + ihl;
    uint16_t plen = (uint16_t)(len - ihl);

    switch (ip->protocol) {
    case IP_PROTO_ICMP: icmp_recv(ip->src, payload, plen); break;
    case IP_PROTO_UDP:  udp_recv(ip->src, ip->dst, payload, plen); break;
    case IP_PROTO_TCP:  tcp_input(ip->src, ip->dst, payload, plen); break;
    default: break;
    }
}

bool ipv4_send(uint32_t dst_ip, uint8_t proto, const void *payload, uint16_t plen)
{
    static uint8_t buf[1500];
    if (sizeof(ip_hdr_t) + plen > sizeof(buf)) return false;

    ip_hdr_t *hdr = (ip_hdr_t *)buf;
    hdr->ver_ihl     = 0x45;
    hdr->dscp_ecn    = 0;
    hdr->total_length= htons((uint16_t)(20 + plen));
    hdr->id          = htons(++s_id);
    hdr->flags_frag  = 0;
    hdr->ttl         = 64;
    hdr->protocol    = proto;
    hdr->checksum    = 0;
    hdr->src         = s_ip;
    hdr->dst         = dst_ip;
    hdr->checksum    = htons(ip_checksum(hdr, 20));
    memcpy(buf + sizeof(ip_hdr_t), payload, plen);

    /* Route: use gateway if not on same subnet. */
    uint32_t next_hop = dst_ip;
    if (s_gateway && (dst_ip & s_netmask) != (s_ip & s_netmask))
        next_hop = s_gateway;

    /* If sending to ourselves or broadcast, use broadcast MAC. */
    uint8_t dst_mac[6];
    if (dst_ip == 0xffffffffU || dst_ip == 0) {
        memcpy(dst_mac, ETH_BROADCAST, 6);
    } else if (!arp_lookup(next_hop, dst_mac)) {
        /* ARP miss — drop. Caller can retry. */
        return false;
    }

    return eth_send(dst_mac, ETHERTYPE_IPV4, buf, (uint16_t)(20 + plen));
}
