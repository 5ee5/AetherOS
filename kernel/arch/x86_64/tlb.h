#ifndef KERNEL_ARCH_X86_64_TLB_H
#define KERNEL_ARCH_X86_64_TLB_H

#include <stdint.h>

/* IPI vector used for cross-CPU TLB invalidation.  Above the scheduler's 0x20
   and 0x22 and the IOAPIC-routed device range. */
#define TLB_SHOOTDOWN_VECTOR 0xfdU

/* Install the shootdown IPI gate.  Call once, on the BSP, after idt_init. */
void tlb_init(void);

/* Tell the shootdown path how many CPUs are running; with one, it is a no-op. */
void tlb_set_cpu_count(uint32_t cpus);

/* Invalidate `virt` on every other CPU and wait for them to acknowledge.
   Must be called with interrupts enabled and no IRQ-disabling lock held. */
void tlb_shootdown_page(uint64_t virt);

#endif
