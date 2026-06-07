#ifndef KERNEL_DRIVERS_PS2KBD_H
#define KERNEL_DRIVERS_PS2KBD_H

#include <stdbool.h>
#include <stdint.h>

/* Initialise PS/2 keyboard (IRQ1, vector 0x21). */
void ps2kbd_init(void);

/* Poll for next ASCII character.  Returns 0 if none available. */
char ps2kbd_getchar(void);

/* Returns true if there is a character waiting. */
bool ps2kbd_ready(void);

#endif
