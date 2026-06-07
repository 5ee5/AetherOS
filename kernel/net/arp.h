#ifndef KERNEL_NET_ARP_H
#define KERNEL_NET_ARP_H

#include <stdbool.h>
#include <stdint.h>

void arp_recv(const void *pkt, uint16_t len);

/* Look up MAC for `ip` (network byte order).  Sends ARP request if unknown.
   Returns true if MAC is in cache.  Returns false and begins request if not. */
bool arp_lookup(uint32_t ip, uint8_t mac_out[6]);

/* Force-insert an entry (used by DHCP/net layer). */
void arp_insert(uint32_t ip, const uint8_t mac[6]);

static const uint8_t ETH_BROADCAST[6] = {0xff,0xff,0xff,0xff,0xff,0xff};

#endif
