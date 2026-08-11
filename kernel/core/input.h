#ifndef KERNEL_CORE_INPUT_H
#define KERNEL_CORE_INPUT_H

#include <stdbool.h>

/* Shared console keyboard input ring, fed by every keyboard source (PS/2 and
   USB HID) and drained by the tty read path (sys_read fd=0). */

/* Push one decoded ASCII character (called from driver context). */
void input_push(char c);

/* Non-blocking: returns the next character, or 0 if the ring is empty. */
char input_getchar(void);

/* Non-blocking look at the next character without consuming it (0 if empty).
   Lets a Ctrl+C watcher inspect the queue without stealing a keystroke from
   whoever is actually reading the console. */
char input_peek(void);

/* True if a character is available. */
bool input_ready(void);

#endif
