#ifndef KERNEL_DRIVERS_E1000_H
#define KERNEL_DRIVERS_E1000_H

#include <stdbool.h>
#include <stdint.h>

/* Initialise the first Intel e1000 NIC found on PCI.
   Registers itself via nic_register().  Returns true if found. */
bool e1000_init(void);

#endif
