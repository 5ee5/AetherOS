#ifndef KERNEL_DRIVERS_XHCI_H
#define KERNEL_DRIVERS_XHCI_H

#include <stdbool.h>
#include <stdint.h>

/* Bring up the first xHCI controller, enumerate a single mass-storage device
   on the first connected root-hub port, and configure its bulk endpoints.
   Returns true if a USB Bulk-Only mass-storage device is ready. Polling only. */
bool xhci_init(void);

/* True once xhci_init has enumerated and configured an MSC device. */
bool xhci_msc_present(void);

/* Control transfer on EP0 of the enumerated device.
   `buf_phys` (may be 0 for no data stage) points into the direct map.
   Returns 0 on success, -1 on error. */
int xhci_control(uint8_t bm_request_type, uint8_t b_request,
                 uint16_t w_value, uint16_t w_index, uint16_t w_length,
                 uint64_t buf_phys);

/* Bulk transfers on the enumerated device's MSC endpoints.
   `buf_phys` points into the direct map. Returns bytes transferred, or -1. */
int xhci_bulk_out(uint64_t buf_phys, uint32_t len);
int xhci_bulk_in(uint64_t buf_phys, uint32_t len);

#endif
