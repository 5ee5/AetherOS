#include "arch/x86_64/idt.h"

#include <stdint.h>

#include "core/panic.h"
#include "core/serial.h"

#define IDT_INTERRUPT_GATE 0x8eU
#define KERNEL_CODE_SELECTOR 0x08U

struct idt_entry {
	uint16_t offset_low;
	uint16_t selector;
	uint8_t ist;
	uint8_t attributes;
	uint16_t offset_mid;
	uint32_t offset_high;
	uint32_t reserved;
} __attribute__((packed));

struct idtr {
	uint16_t limit;
	uint64_t base;
} __attribute__((packed));

extern void (*x86_64_isr_stub_table[])(void);

static struct idt_entry idt[256];

static void lidt(const struct idtr *idtr)
{
	__asm__ volatile("lidt (%0)" : : "r"(idtr) : "memory");
}

static void idt_set_gate(uint8_t vector, void (*handler)(void))
{
	uint64_t address = (uint64_t)(uintptr_t)handler;
	idt[vector].offset_low = (uint16_t)(address & 0xffffU);
	idt[vector].selector = KERNEL_CODE_SELECTOR;
	idt[vector].ist = 0;
	idt[vector].attributes = IDT_INTERRUPT_GATE;
	idt[vector].offset_mid = (uint16_t)((address >> 16U) & 0xffffU);
	idt[vector].offset_high = (uint32_t)((address >> 32U) & 0xffffffffU);
	idt[vector].reserved = 0;
}

void idt_install_gate(uint8_t vector, void (*handler)(void), uint8_t ist)
{
	idt_set_gate(vector, handler);
	idt[vector].ist = ist;
}

void x86_64_idt_init(void)
{
	for (uint8_t vector = 0; vector < 32U; ++vector) {
		idt_set_gate(vector, x86_64_isr_stub_table[vector]);
	}
	idt[8].ist = 1;  /* #DF uses IST1 so a corrupt RSP doesn't triple-fault */

	struct idtr idtr = {
		.limit = sizeof(idt) - 1U,
		.base = (uint64_t)(uintptr_t)idt,
	};
	lidt(&idtr);
}

void x86_64_exception_dispatch(struct x86_64_interrupt_frame *frame)
{
	serial_write("exception vector=");
	serial_write_hex(frame->vector);
	serial_write(" error=");
	serial_write_hex(frame->error_code);
	serial_write(" rip=");
	serial_write_hex(frame->rip);
	serial_write("\n");
	panic("unhandled CPU exception");
}

