#ifndef KERNEL_NET_DNS_H
#define KERNEL_NET_DNS_H

#include <stdbool.h>
#include <stdint.h>

/* Resolve `hostname` to an IPv4 address (network byte order).
   Polls the NIC internally.  Returns true and fills `ip_out` on success. */
bool dns_resolve(const char *hostname, uint32_t *ip_out);

#endif
