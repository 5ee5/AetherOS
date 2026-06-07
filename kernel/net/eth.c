#include "net/eth.h"
#include "net/nic.h"
#include "net/arp.h"
#include "net/ipv4.h"

#include <stdint.h>

#include "lib/string.h"

static uint8_t s_frame[ETH_MAX_FRAME + ETH_HLEN];

bool eth_send(const uint8_t dst_mac[6], uint16_t ethertype,
              const void *payload, uint16_t payload_len)
{
    nic_t *nic = nic_get();
    if (!nic) return false;
    if (payload_len + ETH_HLEN > ETH_MAX_FRAME + ETH_HLEN) return false;

    eth_hdr_t *hdr = (eth_hdr_t *)s_frame;
    memcpy(hdr->dst, dst_mac, 6);
    memcpy(hdr->src, nic->mac, 6);
    hdr->ethertype = htons(ethertype);
    memcpy(s_frame + ETH_HLEN, payload, payload_len);

    uint16_t total = (uint16_t)(ETH_HLEN + payload_len);
    if (total < 60) total = 60; /* minimum Ethernet frame */
    return nic->send(nic, s_frame, total);
}

void eth_recv(const void *frame, uint16_t len)
{
    if (len < ETH_HLEN) return;
    const eth_hdr_t *hdr = (const eth_hdr_t *)frame;
    uint16_t et  = ntohs(hdr->ethertype);
    const uint8_t *payload = (const uint8_t *)frame + ETH_HLEN;
    uint16_t plen = (uint16_t)(len - ETH_HLEN);

    switch (et) {
    case ETHERTYPE_ARP:  arp_recv(payload, plen); break;
    case ETHERTYPE_IPV4: ipv4_recv(hdr->src, payload, plen); break;
    default: break;
    }
}
