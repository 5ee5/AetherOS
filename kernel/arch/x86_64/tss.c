#include "arch/x86_64/tss.h"

#include <stdint.h>

#include "arch/x86_64/gdt.h"
#include "core/serial.h"

extern char kernel_stack_top[];

static struct tss64 tss;
static uint8_t df_stack[4096] __attribute__((aligned(16)));

static void load_tr(uint16_t selector)
{
	__asm__ volatile("ltr %w0" : : "r"(selector) : "memory");
}

void tss_set_rsp0(uint64_t rsp0)
{
	tss.rsp[0] = rsp0;
}

void tss_init(void)
{
	tss.rsp[0] = (uint64_t)(uintptr_t)kernel_stack_top;
	tss.ist[0] = (uint64_t)(uintptr_t)(df_stack + sizeof(df_stack));
	tss.iopb = (uint16_t)sizeof(tss);

	uint16_t selector = gdt_install_tss(&tss, (uint32_t)sizeof(tss));
	load_tr(selector);

	serial_write("kernel: tss rsp0=");
	serial_write_hex(tss.rsp[0]);
	serial_write(" ist1=");
	serial_write_hex(tss.ist[0]);
	serial_write("\n");
}
