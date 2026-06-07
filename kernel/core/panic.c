#include "core/panic.h"

#include "arch/x86_64/io.h"
#include "core/serial.h"

void panic(const char *message)
{
	cpu_cli();
	serial_write("kernel panic: ");
	serial_write(message);
	serial_write("\n");
	for (;;) {
		cpu_halt();
	}
}

