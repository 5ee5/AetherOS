#include "sched/sched.h"

#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/apic.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/tss.h"
#include "core/panic.h"
#include "core/serial.h"
#include "mem/vmm.h"
#include "sched/thread.h"
#include "sync/spinlock.h"

#define MAX_CPUS 64

/* Declared in sched_asm.asm. */
extern void apic_timer_isr(void);
extern void sched_idle_loop(void);

/* ---- Run queue --------------------------------------------------------- */

static spinlock_t rq_lock = SPINLOCK_INIT;
static struct thread *rq_head;
static struct thread *rq_tail;

static void rq_add(struct thread *t)
{
	t->run_next = NULL;
	if (rq_tail) {
		rq_tail->run_next = t;
	} else {
		rq_head = t;
	}
	rq_tail = t;
}

static struct thread *rq_take(void)
{
	if (rq_head == NULL) {
		return NULL;
	}
	struct thread *t = rq_head;
	rq_head = t->run_next;
	if (rq_head == NULL) {
		rq_tail = NULL;
	}
	t->run_next = NULL;
	return t;
}

/* ---- Per-CPU state ----------------------------------------------------- */

static struct thread *cpu_current[MAX_CPUS];
static struct thread *cpu_idle[MAX_CPUS];

/* ---- Public API -------------------------------------------------------- */

void sched_init(void)
{
	/* Install the APIC timer ISR at vector 0x20. */
	idt_install_gate(APIC_TIMER_VECTOR, apic_timer_isr, 0);

	/* Register the BSP as thread 0. */
	struct thread *bsp = thread_create_current();
	uint32_t cpu = lapic_id();
	cpu_current[cpu] = bsp;

	/* Create the BSP idle thread (not added to run queue). */
	cpu_idle[cpu] = thread_create_idle();

	/* Set up per-CPU GS-base data for SYSCALL/SYSRET. */
	cpu_local_init(cpu);

	serial_write("sched: init cpu=");
	serial_write_dec(cpu);
	serial_write(" bsp_tid=");
	serial_write_dec(bsp->tid);
	serial_write("\n");
}

void sched_add(struct thread *t)
{
	spinlock_acquire(&rq_lock);
	rq_add(t);
	spinlock_release(&rq_lock);
}

struct thread *sched_current(void)
{
	return cpu_current[lapic_id()];
}

void sched_wake(struct thread *t)
{
	spinlock_acquire(&rq_lock);
	t->state = THREAD_READY;
	rq_add(t);
	spinlock_release(&rq_lock);
}

uint64_t sched_tick(uint64_t old_rsp)
{
	uint32_t cpu = lapic_id();
	struct thread *cur = cpu_current[cpu];

	cur->rsp = old_rsp;

	lapic_eoi();

	spinlock_acquire(&rq_lock);

	/* Re-enqueue current thread only if it was preempted while RUNNING. */
	if (cur->state == THREAD_RUNNING && !cur->is_idle) {
		cur->state = THREAD_READY;
		rq_add(cur);
	}

	struct thread *next = rq_take();
	if (next == NULL) {
		next = cpu_idle[cpu];
	}
	next->state      = THREAD_RUNNING;
	cpu_current[cpu] = next;

	spinlock_release(&rq_lock);

	/* Update TSS rsp0 and CR3 when entering a user thread. */
	if (next->is_user && next->cr3 != 0) {
		tss_set_rsp0(next->kstack_top);
		cpu_local_data[cpu].kernel_rsp = next->kstack_top;
		vmm_space_switch(next->cr3);
	} else {
		vmm_space_switch(vmm_pml4_phys());
	}

	return next->rsp;
}

void sched_ap_prepare(uint32_t cpu_id)
{
	/* Allocate on the BSP before the AP is started so no alloc races with tests. */
	cpu_idle[cpu_id] = thread_create_idle();
}

void sched_ap_enter(void)
{
	uint32_t cpu = lapic_id();
	/* Idle thread was pre-allocated by the BSP via sched_ap_prepare. */
	struct thread *idle = cpu_idle[cpu];
	if (idle == NULL) {
		/* Fallback: should not happen if smp_init called sched_ap_prepare. */
		idle = thread_create_idle();
		cpu_idle[cpu] = idle;
	}
	cpu_current[cpu] = idle;

	/* Set up per-CPU GS-base data for SYSCALL/SYSRET. */
	cpu_local_init(cpu);

	serial_write("sched: ap cpu=");
	serial_write_dec(cpu);
	serial_write("\n");
}
