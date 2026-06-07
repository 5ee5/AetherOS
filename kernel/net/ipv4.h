#ifndef KERNEL_NET_IPV4_H
#define KERNEL_NET_IPV4_H

#include <stdbool.h>
#include <stdint.h>

#define IP_PROTO_ICMP  1U
#define IP_PROTO_TCP   6U
#define IP_PROTO_UDP   17U

typedef struct {
    uint8_t  ver_ihl;
    uint8_t  dscp_ecn;
    uint16_t total_length;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src;
    uint32_t dst;
} __attribute__((packed)) ip_hdr_t;

/* Called from eth.c when an IPv4 frame arrives. src_mac is the sender MAC. */
void ipv4_recv(const uint8_t src_mac[6], const void *pkt, uint16_t len);

/* Send an IPv4 packet.  dst_ip is in network byte order. */
bool ipv4_send(uint32_t dst_ip, uint8_t proto, const void *payload, uint16_t len);

/* Get/set our IP (network byte order). */
uint32_t ipv4_our_ip(void);
void     ipv4_set_config(uint32_t ip, uint32_t netmask, uint32_t gateway, uint32_t dns);
uint32_t ipv4_gateway(void);
uint32_t ipv4_dns(void);

#endif
