#ifndef KERNEL_DRIVERS_USB_HID_H
#define KERNEL_DRIVERS_USB_HID_H

#include <stdbool.h>

/* Find a USB HID boot-protocol keyboard among the xHCI-enumerated devices,
   put it in boot protocol, and start a poll thread that feeds decoded keys into
   the shared console input ring. Call after usb_init()/vfs_init(). Returns true
   if a keyboard was found and configured. */
bool usb_hid_init(void);

#endif
