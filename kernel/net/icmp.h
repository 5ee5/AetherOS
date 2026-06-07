#ifndef KERNEL_NET_ICMP_H
#define KERNEL_NET_ICMP_H

#include <stdint.h>

void icmp_recv(uint32_t src_ip, const void *pkt, uint16_t len);

#endif
