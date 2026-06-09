#include "arch/x86_64/idt.h"

#include <stdint.h>

#include "core/panic.h"
#include "core/serial.h"
#include "proc/process.h"
#include "sched/sched.h"
#include "sched/thread.h"

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

static uint64_t read_cr2(void)
{
	uint64_t cr2;
	__asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
	return cr2;
}

void x86_64_exception_dispatch(struct x86_64_interrupt_frame *frame)
{
	int user_mode = (frame->cs & 3U) == 3U;

	serial_write("exception vector=");
	serial_write_hex(frame->vector);
	serial_write(" error=");
	serial_write_hex(frame->error_code);
	serial_write(" rip=");
	serial_write_hex(frame->rip);
	serial_write(" cs=");
	serial_write_hex(frame->cs);
	serial_write(" rsp=");
	serial_write_hex(user_mode ? frame->rsp : 0);
	if (frame->vector == 14U) {
		serial_write(" cr2=");
		serial_write_hex(read_cr2());
	}
	serial_write(user_mode ? " [user]\n" : " [kernel]\n");
	serial_write("  rax="); serial_write_hex(frame->rax);
	serial_write(" rbx="); serial_write_hex(frame->rbx);
	serial_write(" rcx="); serial_write_hex(frame->rcx);
	serial_write(" rdx="); serial_write_hex(frame->rdx);
	serial_write("\n  rsi="); serial_write_hex(frame->rsi);
	serial_write(" rdi="); serial_write_hex(frame->rdi);
	serial_write(" rbp="); serial_write_hex(frame->rbp);
	serial_write("\n  r8=");  serial_write_hex(frame->r8);
	serial_write(" r9=");  serial_write_hex(frame->r9);
	serial_write(" r10="); serial_write_hex(frame->r10);
	serial_write(" r11="); serial_write_hex(frame->r11);
	serial_write("\n  r12="); serial_write_hex(frame->r12);
	serial_write(" r13="); serial_write_hex(frame->r13);
	serial_write(" r14="); serial_write_hex(frame->r14);
	serial_write(" r15="); serial_write_hex(frame->r15);
	serial_write("\n");

	if (user_mode) {
		struct process *proc = sched_current_process();
		if (proc) {
			serial_write("segfault: killing pid=");
			serial_write_dec(proc->pid);
			serial_write("\n");
			process_kill(proc);
		}
		struct thread *t = sched_current();
		if (t) {
			__asm__ volatile("cli" ::: "memory");
			t->state = THREAD_DEAD;
			__asm__ volatile("int $0x20" ::: "memory");
		}
		for (;;) __asm__ volatile("hlt");
	}

	panic("unhandled CPU exception");
}

