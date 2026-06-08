#ifndef KERNEL_CORE_SERIAL_H
#define KERNEL_CORE_SERIAL_H

#include <stdint.h>

void serial_init(void);
void serial_write(const char *text);
void serial_write_char(char c);
void serial_write_hex(uint64_t value);
void serial_write_dec(uint64_t value);

/* Non-blocking read from COM1 RX.  Returns 0 if no data available. */
char serial_read_char(void);

#endif

