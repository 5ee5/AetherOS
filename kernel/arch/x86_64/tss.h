#ifndef KERNEL_ARCH_X86_64_TSS_H
#define KERNEL_ARCH_X86_64_TSS_H

#include <stdint.h>

struct tss64 {
	uint32_t reserved0;
	uint64_t rsp[3];
	uint64_t reserved1;
	uint64_t ist[7];
	uint64_t reserved2;
	uint16_t reserved3;
	uint16_t iopb;
} __attribute__((packed));

/* BSP: sets up slot 0's TSS and loads TR.  Runs before the LAPIC is mapped, so
   the CPU-to-TSS binding is completed later by tss_bind_current(). */
void tss_init(void);

/* AP: sets up `slot`'s TSS, loads TR, and binds it to the running CPU.  The
   CPU's own GDT must already be loaded (x86_64_gdt_init_cpu). */
void tss_init_ap(uint32_t slot);

/* Bind the running CPU's LAPIC id to the TSS in `slot`.  The BSP calls this once
   the LAPIC is up; tss_set_rsp0 needs the mapping to find its own TSS. */
void tss_bind_current(uint32_t slot);

/* Sets rsp[0] in the *running* CPU's TSS. */
void tss_set_rsp0(uint64_t rsp0);

#endif
