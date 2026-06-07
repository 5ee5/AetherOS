#ifndef KERNEL_DRIVERS_AHCI_H
#define KERNEL_DRIVERS_AHCI_H

#include <stdbool.h>
#include <stdint.h>

/* Find the AHCI controller via PCI and initialise it.
   Returns true if at least one disk was found. */
bool ahci_init(void);

/* Read `count` 512-byte sectors from `port` starting at `lba` into `buf`.
   `buf` must be physically contiguous (one PMM page, max 4 KiB = 8 sectors).
   Returns true on success. */
bool ahci_read_sectors(uint8_t port, uint64_t lba, uint16_t count, void *buf);

/* Return the port index of the first present disk, or -1 if none. */
int ahci_first_disk(void);

#endif
