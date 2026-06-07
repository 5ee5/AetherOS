#ifndef KERNEL_NET_CHECKSUM_H
#define KERNEL_NET_CHECKSUM_H

#include <stdint.h>

/* Standard one's-complement internet checksum.
   Returns the checksum in network byte order (ready to store in header).
   Pass initial=0 for a fresh computation. */
uint16_t ip_checksum(const void *data, uint32_t len);

/* UDP/TCP pseudo-header checksum.
   Returns 0 if the segment is valid (checksum stored in segment is correct). */
uint16_t ip_pseudo_checksum(uint32_t src_ip, uint32_t dst_ip,
                             uint8_t proto, const void *segment, uint16_t seg_len);

#endif
