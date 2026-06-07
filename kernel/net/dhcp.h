#ifndef KERNEL_NET_DHCP_H
#define KERNEL_NET_DHCP_H

#include <stdbool.h>
#include <stdint.h>

/* Run the DHCP DISCOVER→OFFER→REQUEST→ACK sequence.
   Polls the NIC internally.  Returns true if configuration was obtained. */
bool dhcp_discover(void);

#endif
