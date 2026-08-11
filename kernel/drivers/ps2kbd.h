#ifndef KERNEL_DRIVERS_PS2KBD_H
#define KERNEL_DRIVERS_PS2KBD_H

#include <stdbool.h>
#include <stdint.h>

/* Initialise PS/2 keyboard (IRQ1, vector 0x21). */
void ps2kbd_init(void);

/* Keystrokes are pushed into the shared console ring (core/input.h); there is no
   driver-private buffer to poll. */

#endif
