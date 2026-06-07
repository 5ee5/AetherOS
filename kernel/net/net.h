#ifndef KERNEL_NET_NET_H
#define KERNEL_NET_NET_H

#include <stdbool.h>
#include <stdint.h>

/* Initialise networking: discover NIC, run DHCP, start polling thread.
   Returns true if a NIC was found (DHCP success optional). */
bool net_init(void);

/* Process one incoming frame (called from the polling thread). */
void net_poll(void);

#endif
