#include "arch/x86_64/gdt.h"

#include <stdint.h>

struct gdtr {
	uint16_t limit;
	uint64_t base;
} __attribute__((packed));

extern void x86_64_gdt_load(const struct gdtr *gdtr);

static uint64_t gdt[7];

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

uint16_t gdt_install_tss(void *tss, uint32_t tss_size)
{
	uint64_t base = (uint64_t)(uintptr_t)tss;
	uint64_t limit = (uint64_t)tss_size - 1U;

	/* 64-bit TSS system descriptor: lower 8 bytes */
	gdt[3] = (limit & 0xffffULL)
		| ((base & 0x00ffffffULL) << 16U)
		| (0x89ULL << 40U)   /* P=1, DPL=0, S=0, Type=9 (64-bit TSS) */
		| (((limit >> 16U) & 0xfULL) << 48U)
		| (((base >> 24U) & 0xffULL) << 56U);

	/* upper 8 bytes: base[63:32] */
	gdt[4] = (base >> 32U) & 0xffffffffULL;

	return 0x18U; /* selector for gdt[3] */
}

void x86_64_gdt_init(void)
{
	gdt[0] = 0;
	gdt[1] = gdt_descriptor(0, 0xfffff, 0x9a, 0x0a);  /* kernel code */
	gdt[2] = gdt_descriptor(0, 0xfffff, 0x92, 0x0c);  /* kernel data */
	/* gdt[3..4]: TSS, filled by gdt_install_tss() */
	gdt[5] = gdt_descriptor(0, 0xfffff, 0xf2, 0x0c);  /* user data,  DPL=3 */
	gdt[6] = gdt_descriptor(0, 0xfffff, 0xfa, 0x0a);  /* user code64, DPL=3 */

	struct gdtr gdtr = {
		.limit = sizeof(gdt) - 1U,
		.base = (uint64_t)(uintptr_t)gdt,
	};
	x86_64_gdt_load(&gdtr);
}

