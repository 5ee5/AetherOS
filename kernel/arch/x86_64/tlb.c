#include "arch/x86_64/tlb.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/apic.h"
#include "arch/x86_64/idt.h"
#include "core/serial.h"
#include "sync/spinlock.h"

/* Cross-CPU TLB invalidation.
 *
 * vmm_unmap and vmm_protect used to issue invlpg on the local CPU only, and
 * nothing at all for other cores.  Kernel-half page tables are shared by every
 * address space, so another CPU could keep writing through a stale writable
 * entry for a page that had already been unmapped and handed back to the PMM.
 * That is a straightforward use-after-free; it was invisible only because no AP
 * ever ran a thread.
 */

extern void tlb_shootdown_isr(void);

/* The page every other CPU is being told to invalidate.  Written under
   shoot_lock, read by the IPI handlers. */
static volatile uint64_t s_shoot_addr;

/* Number of CPUs that have yet to acknowledge the current shootdown. */
static volatile uint32_t s_shoot_pending;

/* One shootdown in flight at a time: there is a single address slot.
   Deliberately NOT irqsave -- see the interrupt discipline note below. */
static spinlock_t shoot_lock;

/* CPUs actually running.  1 until smp_init reports otherwise, so every
   shootdown before the APs are up is a no-op. */
static volatile uint32_t s_cpus_online = 1U;

void tlb_init(void)
{
	idt_install_gate(TLB_SHOOTDOWN_VECTOR, tlb_shootdown_isr, 0);
}

void tlb_set_cpu_count(uint32_t cpus)
{
	s_cpus_online = (cpus == 0U) ? 1U : cpus;
}

/* Called from the IPI stub on every other CPU. */
void tlb_shootdown_handler(void)
{
	__asm__ volatile("invlpg (%0)" : : "r"((uintptr_t)s_shoot_addr) : "memory");
	__atomic_fetch_sub(&s_shoot_pending, 1U, __ATOMIC_SEQ_CST);
	lapic_eoi();
}

static bool interrupts_enabled(void)
{
	uint64_t flags;
	__asm__ volatile("pushfq; pop %0" : "=r"(flags));
	return (flags & 0x200ULL) != 0ULL;
}

void tlb_shootdown_page(uint64_t virt)
{
	if (s_cpus_online <= 1U) {
		return;   /* uniprocessor: the local invlpg already did the job */
	}

	/* Interrupt discipline, and why nothing here disables interrupts:
	   this CPU spins until the others acknowledge, and they can only do that
	   by taking an interrupt.  If we waited with interrupts off while another
	   CPU was doing the same, each would be waiting for an acknowledgement the
	   other could never deliver.  Spinning for shoot_lock has the same hazard,
	   so that acquire is not irqsave either -- a CPU queued behind the lock
	   stays able to service the shootdown it is queued behind.
	   Callers therefore must not hold an IRQ-disabling lock across this; the
	   VMM releases vmm_lock before calling. */
	if (!interrupts_enabled()) {
		/* Cannot safely wait for acks here: the CPUs we would wait on may be
		   waiting on us.  The caller's local invlpg still ran, so this is a
		   missed remote flush, not a missed flush.  In-tree this is only
		   reached from the kernel test suites, which run with IF clear and
		   whose test pages no other CPU touches; every runtime caller
		   (vmm_unmap/vmm_protect from thread context) has interrupts on.
		   Warn once rather than on every call. */
		static bool warned;
		if (!warned) {
			warned = true;
			serial_write("tlb: remote shootdown skipped, interrupts disabled "
				     "(expected during ktest only)\n");
		}
		return;
	}

	spinlock_acquire(&shoot_lock);

	s_shoot_addr = virt;
	__atomic_store_n(&s_shoot_pending, s_cpus_online - 1U, __ATOMIC_SEQ_CST);

	lapic_broadcast_ipi(TLB_SHOOTDOWN_VECTOR);

	/* Bounded: a CPU wedged with interrupts off must not hang the machine. */
	for (uint64_t spins = 0; spins < 100000000ULL; ++spins) {
		if (__atomic_load_n(&s_shoot_pending, __ATOMIC_SEQ_CST) == 0U) {
			break;
		}
		__asm__ volatile("pause");
	}

	spinlock_release(&shoot_lock);
}
