#include "net/arp.h"
#include "net/eth.h"
#include "net/ipv4.h"

#include <stdint.h>

#include "lib/string.h"
#include "net/nic.h"

#define ARP_TABLE_SIZE 32

#define ARP_HW_ETHERNET 0x0001U
#define ARP_PROTO_IP    0x0800U
#define ARP_OP_REQUEST  1U
#define ARP_OP_REPLY    2U

typedef struct {
    uint16_t hw_type;     /* big-endian */
    uint16_t proto_type;  /* big-endian */
    uint8_t  hw_len;
    uint8_t  proto_len;
    uint16_t op;          /* big-endian */
    uint8_t  sender_mac[6];
    uint32_t sender_ip;   /* big-endian */
    uint8_t  target_mac[6];
    uint32_t target_ip;   /* big-endian */
} __attribute__((packed)) arp_pkt_t;

typedef struct {
    uint32_t ip;
    uint8_t  mac[6];
    bool     valid;
} arp_entry_t;

static arp_entry_t s_table[ARP_TABLE_SIZE];

static arp_entry_t *find_entry(uint32_t ip)
{
    for (int i = 0; i < ARP_TABLE_SIZE; ++i)
        if (s_table[i].valid && s_table[i].ip == ip) return &s_table[i];
    return NULL;
}

void arp_insert(uint32_t ip, const uint8_t mac[6])
{
    arp_entry_t *e = find_entry(ip);
    if (!e) {
        for (int i = 0; i < ARP_TABLE_SIZE; ++i) {
            if (!s_table[i].valid) { e = &s_table[i]; break; }
        }
    }
    if (!e) e = &s_table[0]; /* overwrite oldest */
    e->ip    = ip;
    e->valid = true;
    memcpy(e->mac, mac, 6);
}

void arp_recv(const void *pkt, uint16_t len)
{
    if (len < sizeof(arp_pkt_t)) return;
    const arp_pkt_t *a = (const arp_pkt_t *)pkt;
    if (ntohs(a->hw_type)    != ARP_HW_ETHERNET) return;
    if (ntohs(a->proto_type) != ARP_PROTO_IP)    return;

    /* Cache the sender. */
    arp_insert(a->sender_ip, a->sender_mac);

    if (ntohs(a->op) == ARP_OP_REQUEST) {
        /* Reply if it's for our IP. */
        uint32_t our_ip = ipv4_our_ip();
        if (our_ip == 0 || a->target_ip != our_ip) return;

        nic_t *nic = nic_get();
        if (!nic) return;

        arp_pkt_t reply;
        reply.hw_type    = htons(ARP_HW_ETHERNET);
        reply.proto_type = htons(ARP_PROTO_IP);
        reply.hw_len     = 6;
        reply.proto_len  = 4;
        reply.op         = htons(ARP_OP_REPLY);
        memcpy(reply.sender_mac, nic->mac, 6);
        reply.sender_ip  = our_ip;
        memcpy(reply.target_mac, a->sender_mac, 6);
        reply.target_ip  = a->sender_ip;
        eth_send(a->sender_mac, ETHERTYPE_ARP, &reply, sizeof(reply));
    }
}

bool arp_lookup(uint32_t ip, uint8_t mac_out[6])
{
    arp_entry_t *e = find_entry(ip);
    if (e) { memcpy(mac_out, e->mac, 6); return true; }

    /* Send ARP request. */
    nic_t *nic = nic_get();
    if (!nic) return false;

    arp_pkt_t req;
    req.hw_type    = htons(ARP_HW_ETHERNET);
    req.proto_type = htons(ARP_PROTO_IP);
    req.hw_len     = 6;
    req.proto_len  = 4;
    req.op         = htons(ARP_OP_REQUEST);
    memcpy(req.sender_mac, nic->mac, 6);
    req.sender_ip  = ipv4_our_ip();
    memset(req.target_mac, 0, 6);
    req.target_ip  = ip;
    eth_send(ETH_BROADCAST, ETHERTYPE_ARP, &req, sizeof(req));
    return false;
}
