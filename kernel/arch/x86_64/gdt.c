#include "arch/x86_64/gdt.h"

#include <stdint.h>

#include "arch/x86_64/cpu.h"
#include "core/panic.h"

struct gdtr {
	uint16_t limit;
	uint64_t base;
} __attribute__((packed));

extern void x86_64_gdt_load(const struct gdtr *gdtr);

/* One GDT per CPU.  The entries are identical except for the TSS descriptor at
   index 3/4, which must point at that CPU's own TSS -- a single shared GDT can
   only ever describe one TSS, so every AP would have had to share the BSP's
   (or run with none, which is what happened: APs never executed ltr at all).
   Keeping the layout identical means the selectors stay compile-time constants
   and SYSCALL's STAR encoding is valid on every CPU. */
static uint64_t gdt[MAX_CPUS][7];

static uint64_t gdt_descriptor(uint32_t base, uint32_t limit, uint8_t access,
	uint8_t flags)
{
	uint64_t descriptor = 0;
	descriptor |= (uint64_t)(limit & 0xffffU);
	descriptor |= (uint64_t)(base & 0xffffffU) << 16U;
	descriptor |= (uint64_t)access << 40U;
	descriptor |= (uint64_t)((limit >> 16U) & 0x0fU) << 48U;
	descriptor |= (uint64_t)(flags & 0x0fU) << 52U;
	descriptor |= (uint64_t)((base >> 24U) & 0xffU) << 56U;
	return descriptor;
}

uint16_t gdt_install_tss(uint32_t slot, void *tss, uint32_t tss_size)
{
	if (slot >= MAX_CPUS) {
		panic("gdt: TSS slot out of range");
	}

	uint64_t base = (uint64_t)(uintptr_t)tss;
	uint64_t limit = (uint64_t)tss_size - 1U;

	/* 64-bit TSS system descriptor: lower 8 bytes */
	gdt[slot][3] = (limit & 0xffffULL)
		| ((base & 0x00ffffffULL) << 16U)
		| (0x89ULL << 40U)   /* P=1, DPL=0, S=0, Type=9 (64-bit TSS) */
		| (((limit >> 16U) & 0xfULL) << 48U)
		| (((base >> 24U) & 0xffULL) << 56U);

	/* upper 8 bytes: base[63:32] */
	gdt[slot][4] = (base >> 32U) & 0xffffffffULL;

	return 0x18U; /* selector for gdt[slot][3] */
}

/* Build and load this CPU's GDT.  Called on the BSP with slot 0 from
   x86_64_gdt_init, and on each AP with its own slot from ap_entry. */
void x86_64_gdt_init_cpu(uint32_t slot)
{
	if (slot >= MAX_CPUS) {
		panic("gdt: CPU slot out of range");
	}

	gdt[slot][0] = 0;
	gdt[slot][1] = gdt_descriptor(0, 0xfffff, 0x9a, 0x0a);  /* kernel code */
	gdt[slot][2] = gdt_descriptor(0, 0xfffff, 0x92, 0x0c);  /* kernel data */
	/* gdt[slot][3..4]: TSS, filled by gdt_install_tss() */
	gdt[slot][5] = gdt_descriptor(0, 0xfffff, 0xf2, 0x0c);  /* user data,  DPL=3 */
	gdt[slot][6] = gdt_descriptor(0, 0xfffff, 0xfa, 0x0a);  /* user code64, DPL=3 */

	struct gdtr gdtr = {
		.limit = sizeof(gdt[slot]) - 1U,
		.base = (uint64_t)(uintptr_t)gdt[slot],
	};
	x86_64_gdt_load(&gdtr);
}

void x86_64_gdt_init(void)
{
	x86_64_gdt_init_cpu(0);   /* the BSP always takes slot 0 */
}
