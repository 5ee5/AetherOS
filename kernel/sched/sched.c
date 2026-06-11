#include "sched/sched.h"

#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/apic.h"
#include "proc/process.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/tss.h"
#include "core/panic.h"
#include "core/serial.h"
#include "mem/heap.h"
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

static volatile uint64_t g_ticks;
static struct thread *sleep_head;  /* protected by rq_lock */

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

/* Dead thread waiting to be freed on the next tick (we can't free a thread's
   kstack while still running on it; deferring one tick ensures we've switched). */
static struct thread *reap_pending[MAX_CPUS];

/* The executing CPU's LAPIC ID, validated as a per-CPU array index.  smp_init
   refuses to start an AP whose LAPIC ID is >= MAX_CPUS and sched_init rejects
   such a BSP, so an out-of-range ID reaching here is a bug rather than normal
   topology — fail loudly instead of corrupting memory past the arrays. */
static uint32_t current_cpu_id(void)
{
	uint32_t id = lapic_id();
	if (id >= MAX_CPUS) {
		panic("sched: LAPIC ID exceeds MAX_CPUS");
	}
	return id;
}

/* ---- Public API -------------------------------------------------------- */

void sched_init(void)
{
	/* Install the APIC timer ISR at vector 0x20. */
	idt_install_gate(APIC_TIMER_VECTOR, apic_timer_isr, 0);

	/* Register the BSP as thread 0. */
	struct thread *bsp = thread_create_current();
	uint32_t cpu = current_cpu_id();
	cpu_current[cpu] = bsp;
	bsp->on_cpu = true;

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
	return cpu_current[current_cpu_id()];
}

struct process *sched_current_process(void)
{
	struct thread *t = sched_current();
	if (!t) return NULL;
	return (struct process *)t->process;
}

void sched_wake(struct thread *t)
{
	/* Wait until the thread has left its CPU before making it runnable.  A
	   thread blocks by setting BLOCKED, dropping its lock, then yielding via
	   int $0x20; a waker on another CPU can run in that window.  Enqueuing the
	   thread before it has saved its context would let a third CPU resume it on
	   its still-live stack.  sched_tick clears on_cpu once the context is saved;
	   callers only wake already-blocked threads, so this spins briefly. */
	while (t->on_cpu) {
		__asm__ volatile("pause" ::: "memory");
	}
	spinlock_acquire(&rq_lock);
	t->state = THREAD_READY;
	rq_add(t);
	spinlock_release(&rq_lock);
}

uint64_t sched_tick(uint64_t old_rsp)
{
	uint32_t cpu = current_cpu_id();

	/* Free any thread that died during the previous tick.  We deferred this
	   because we couldn't kfree a stack while still running on it. */
	if (reap_pending[cpu] != NULL) {
		struct thread *dead = reap_pending[cpu];
		reap_pending[cpu] = NULL;
		/* Tear down the user address space here rather than in sys_exit:
		   by now CR3 has been switched to another thread, so the dead
		   thread's PML4 and page tables are no longer the active CR3 and
		   are safe to free.  Covers exit, kill, and segfault uniformly. */
		if (dead->is_user && dead->cr3 != 0) {
			vmm_space_destroy(dead->cr3);
			dead->cr3 = 0;
		}
		kfree(dead->kstack);
		kfree(dead);
	}

	struct thread *cur = cpu_current[cpu];
	cur->rsp = old_rsp;

	lapic_eoi();

	spinlock_acquire(&rq_lock);

	/* Advance tick counter and wake any expired sleepers. */
	g_ticks++;
	struct thread **sp = &sleep_head;
	while (*sp) {
		struct thread *st = *sp;
		if (st->sleep_deadline <= g_ticks) {
			*sp = st->sleep_next;
			st->sleep_next = NULL;
			st->state = THREAD_READY;
			rq_add(st);
		} else {
			sp = &st->sleep_next;
		}
	}

	/* Re-enqueue current thread only if it was preempted while RUNNING. */
	if (cur->state == THREAD_RUNNING && !cur->is_idle) {
		cur->state = THREAD_READY;
		rq_add(cur);
	} else if (cur->state == THREAD_DEAD) {
		/* Don't re-queue; schedule for deferred free next tick. */
		reap_pending[cpu] = cur;
	}

	struct thread *next = rq_take();
	if (next == NULL) {
		next = cpu_idle[cpu];
	}
	next->state      = THREAD_RUNNING;
	/* Hand off the on_cpu marker: cur's context was saved above, so clearing
	   its flag lets a waker on another CPU safely enqueue it; mark the incoming
	   thread as running on this CPU.  (See sched_wake.) */
	if (cur != next) {
		cur->on_cpu = false;
	}
	next->on_cpu     = true;
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

void sched_sleep_ms(uint64_t ms)
{
	struct thread *t = sched_current();
	if (!t || t->is_idle) return;

	spinlock_acquire(&rq_lock);
	t->sleep_deadline = g_ticks + ms;
	t->state          = THREAD_BLOCKED;
	t->sleep_next     = sleep_head;
	sleep_head        = t;
	spinlock_release(&rq_lock);

	__asm__ volatile("cli" ::: "memory");
	__asm__ volatile("int $0x20" ::: "memory");
	__asm__ volatile("sti" ::: "memory");
}

void sched_ap_prepare(uint32_t cpu_id)
{
	if (cpu_id >= MAX_CPUS) {
		return;
	}
	/* Allocate on the BSP before the AP is started so no alloc races with tests. */
	cpu_idle[cpu_id] = thread_create_idle();
}

void sched_ap_enter(void)
{
	uint32_t cpu = current_cpu_id();
	/* Idle thread was pre-allocated by the BSP via sched_ap_prepare. */
	struct thread *idle = cpu_idle[cpu];
	if (idle == NULL) {
		/* Fallback: should not happen if smp_init called sched_ap_prepare. */
		idle = thread_create_idle();
		cpu_idle[cpu] = idle;
	}
	cpu_current[cpu] = idle;
	idle->on_cpu = true;

	/* Set up per-CPU GS-base data for SYSCALL/SYSRET. */
	cpu_local_init(cpu);

	serial_write("sched: ap cpu=");
	serial_write_dec(cpu);
	serial_write("\n");
}
