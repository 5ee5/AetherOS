#include "core/serial.h"

#include "arch/x86_64/io.h"
#include "sync/spinlock.h"

#define COM1 0x3f8U

/* Ring buffer for desktop log window. */
#define LOG_BUF_SZ 4096U
static char     s_log_buf[LOG_BUF_SZ];
static uint32_t s_log_write;
static uint32_t s_log_read;
/* Guards the log ring indices against concurrent writers/readers (any CPU). */
static spinlock_t s_log_lock = SPINLOCK_INIT;

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

	/* Feed into log ring buffer (skip \r). Lock only the index update — the
	   tiny critical section keeps the serial-in-panic deadlock window minimal. */
	if (c != '\r') {
		uint64_t f = spinlock_acquire_irqsave(&s_log_lock);
		s_log_buf[s_log_write % LOG_BUF_SZ] = c;
		s_log_write++;
		/* If we lapped the read pointer, advance it to drop oldest data. */
		if (s_log_write - s_log_read > LOG_BUF_SZ)
			s_log_read = s_log_write - LOG_BUF_SZ;
		spinlock_release_irqrestore(&s_log_lock, f);
	}
}

uint32_t serial_log_read(char *buf, uint32_t size)
{
	uint64_t f = spinlock_acquire_irqsave(&s_log_lock);
	uint32_t avail = s_log_write - s_log_read;
	if (avail > size) avail = size;
	for (uint32_t i = 0; i < avail; i++)
		buf[i] = s_log_buf[(s_log_read + i) % LOG_BUF_SZ];
	s_log_read += avail;
	spinlock_release_irqrestore(&s_log_lock, f);
	return avail;
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

