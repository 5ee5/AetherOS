#include "arch/x86_64/tss.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/apic.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/gdt.h"
#include "core/panic.h"
#include "core/serial.h"

extern char kernel_stack_top[];

/* One TSS and one double-fault stack per CPU.  A shared TSS cannot work on SMP:
   rsp[0] is the stack the CPU switches to on a ring3->ring0 transition, so two
   CPUs sharing one would land on the same kernel stack the moment both took an
   interrupt from user mode. */
static struct tss64 tss_pool[MAX_CPUS];
static uint8_t df_stacks[MAX_CPUS][4096] __attribute__((aligned(16)));

/* LAPIC id -> that CPU's TSS.  tss_set_rsp0 runs from the scheduler on whichever
   CPU is switching, and must reach its own TSS, not the BSP's. */
static struct tss64 *cpu_tss[MAX_CPUS];

static void load_tr(uint16_t selector)
{
	__asm__ volatile("ltr %w0" : : "r"(selector) : "memory");
}

static uint64_t current_rsp(void)
{
	uint64_t rsp;
	__asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
	return rsp;
}

void tss_set_rsp0(uint64_t rsp0)
{
	uint32_t id = lapic_id();
	if (id >= MAX_CPUS) {
		return;
	}
	struct tss64 *t = cpu_tss[id];
	if (t != NULL) {
		t->rsp[0] = rsp0;
	}
}

/* Set up slot `slot`'s TSS, install its GDT descriptor, load TR, and bind it to
   the CPU currently executing.  The GDT for this slot must already be loaded. */
static void tss_setup(uint32_t slot, uint64_t rsp0, bool bind_lapic)
{
	if (slot >= MAX_CPUS) {
		panic("tss: CPU slot out of range");
	}

	struct tss64 *t = &tss_pool[slot];
	t->rsp[0] = rsp0;
	t->ist[0] = (uint64_t)(uintptr_t)(df_stacks[slot] + sizeof(df_stacks[slot]));
	t->iopb = (uint16_t)sizeof(*t);

	load_tr(gdt_install_tss(slot, t, (uint32_t)sizeof(*t)));

	/* The BSP runs this before the LAPIC is mapped, so it binds later via
	   tss_bind_current(); an AP already has a usable LAPIC and binds here. */
	if (bind_lapic) {
		tss_bind_current(slot);
	}
}

void tss_init(void)
{
	tss_setup(0, (uint64_t)(uintptr_t)kernel_stack_top, false);

	serial_write("kernel: tss rsp0=");
	serial_write_hex(tss_pool[0].rsp[0]);
	serial_write(" ist1=");
	serial_write_hex(tss_pool[0].ist[0]);
	serial_write("\n");
}

void tss_bind_current(uint32_t slot)
{
	uint32_t id = lapic_id();
	if (slot >= MAX_CPUS || id >= MAX_CPUS) {
		panic("tss: CPU slot or LAPIC id out of range");
	}
	cpu_tss[id] = &tss_pool[slot];
}

void tss_init_ap(uint32_t slot)
{
	/* rsp0 is seeded with this AP's current stack and then maintained by the
	   scheduler, which writes it before dispatching any user thread. */
	tss_setup(slot, current_rsp(), true);
}
