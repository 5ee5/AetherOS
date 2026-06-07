#ifndef KERNEL_NET_ETH_H
#define KERNEL_NET_ETH_H

#include <stdbool.h>
#include <stdint.h>

#define ETH_HLEN      14U
#define ETH_MAX_FRAME 1514U
#define ETHERTYPE_ARP  0x0806U
#define ETHERTYPE_IPV4 0x0800U

typedef struct {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t ethertype; /* big-endian */
} __attribute__((packed)) eth_hdr_t;

/* Send a raw Ethernet frame.  `payload` follows directly after the header.
   `ethertype` is host-byte-order (will be byte-swapped). */
bool eth_send(const uint8_t dst_mac[6], uint16_t ethertype,
              const void *payload, uint16_t payload_len);

/* Called from net_poll() with a full frame (including Ethernet header). */
void eth_recv(const void *frame, uint16_t len);

/* Byte-swap helpers. */
static inline uint16_t htons(uint16_t x) { return (uint16_t)((x << 8) | (x >> 8)); }
static inline uint16_t ntohs(uint16_t x) { return htons(x); }
static inline uint32_t htonl(uint32_t x) {
    return ((x & 0xff000000U) >> 24) | ((x & 0x00ff0000U) >> 8)
         | ((x & 0x0000ff00U) << 8)  | ((x & 0x000000ffU) << 24);
}
static inline uint32_t ntohl(uint32_t x) { return htonl(x); }

#endif
