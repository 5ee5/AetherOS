#ifndef KERNEL_DRIVERS_XHCI_H
#define KERNEL_DRIVERS_XHCI_H

#include <stdbool.h>
#include <stdint.h>

/* Bring up the first xHCI controller and enumerate every device on a connected
   root-hub port, configuring each device's endpoints. Polling only; no IRQ.
   Returns the number of devices enumerated (0 if none / no controller). */
int xhci_init(void);

/* Find an enumerated device by its (first) interface class/subclass/protocol.
   Returns a device handle (>= 0) or -1. E.g. mass storage = (0x08,0x06,0x50),
   HID boot keyboard = (0x03,0x01,0x01). */
int xhci_find_device(uint8_t iclass, uint8_t isubclass, uint8_t iproto);

/* Control transfer on device `dev` EP0. `buf_phys` (0 for no data stage) points
   into the direct map. Returns 0 on success, -1 on error. */
int xhci_control(int dev, uint8_t bm_request_type, uint8_t b_request,
                 uint16_t w_value, uint16_t w_index, uint16_t w_length,
                 uint64_t buf_phys);

/* Bulk / interrupt transfers on device `dev`'s discovered endpoints.
   `buf_phys` points into the direct map. Returns bytes transferred, or -1. */
int xhci_bulk_out(int dev, uint64_t buf_phys, uint32_t len);
int xhci_bulk_in(int dev, uint64_t buf_phys, uint32_t len);

/* Non-blocking interrupt-IN poll (single outstanding TRB): returns bytes on a
   completed report, -2 if still pending, -1 on error. */
int xhci_intr_poll(int dev, uint64_t buf_phys, uint32_t len);

#endif
