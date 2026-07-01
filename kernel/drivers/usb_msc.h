#ifndef KERNEL_DRIVERS_USB_MSC_H
#define KERNEL_DRIVERS_USB_MSC_H

#include <stdbool.h>
#include <stdint.h>

/* Bring up USB storage: initialize xHCI, then probe the enumerated device with
   SCSI (TEST UNIT READY / READ CAPACITY). Returns true if a 512-byte-sector LUN
   is ready. Call after ahci_init()/nvme_init(), before vfs_init(). */
bool usb_init(void);

/* -1 if no USB LUN, else 0 (mirrors ahci_first_disk / nvme_first_ns). */
int usb_msc_first(void);

/* Sector I/O (512-byte sectors, count 1..8). `buf` must be direct-mapped. */
bool usb_msc_read_sectors(uint64_t lba, uint16_t count, void *buf);
bool usb_msc_write_sectors(uint64_t lba, uint16_t count, const void *buf);

#endif
