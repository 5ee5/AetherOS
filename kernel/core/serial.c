#include "core/serial.h"

#include "arch/x86_64/io.h"

#define COM1 0x3f8U

static int serial_ready;

static int transmit_empty(void)
{
	return (inb(COM1 + 5U) & 0x20U) != 0;
}

static int receive_ready(void)
{
	return (inb(COM1 + 5U) & 0x01U) != 0;
}

char serial_read_char(void)
{
	if (!serial_ready || !receive_ready()) return 0;
	return (char)inb(COM1);
}

void serial_init(void)
{
	outb(COM1 + 1U, 0x00);
	outb(COM1 + 3U, 0x80);
	outb(COM1 + 0U, 0x03);
	outb(COM1 + 1U, 0x00);
	outb(COM1 + 3U, 0x03);
	outb(COM1 + 2U, 0xc7);
	outb(COM1 + 4U, 0x0b);
	serial_ready = 1;
}

void serial_write_char(char c)
{
	if (!serial_ready) {
		return;
	}
	while (!transmit_empty()) {
	}
	if (c == '\n') {
		outb(COM1, '\r');
		while (!transmit_empty()) {
		}
	}
	outb(COM1, (uint8_t)c);
}

void serial_write(const char *text)
{
	while (*text != '\0') {
		serial_write_char(*text++);
	}
}

void serial_write_hex(uint64_t value)
{
	static const char digits[] = "0123456789abcdef";

	serial_write("0x");
	for (int shift = 60; shift >= 0; shift -= 4) {
		serial_write_char(digits[(value >> (uint32_t)shift) & 0xfU]);
	}
}

void serial_write_dec(uint64_t value)
{
	char buffer[21];
	uint64_t index = sizeof(buffer);
	buffer[--index] = '\0';

	if (value == 0) {
		serial_write_char('0');
		return;
	}

	while (value != 0 && index != 0) {
		buffer[--index] = (char)('0' + (value % 10U));
		value /= 10U;
	}

	serial_write(&buffer[index]);
}

