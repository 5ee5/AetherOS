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

/* Scheduler entries, not milliseconds: this handler is entered by the hardware
   timer on every CPU *and* by every software yield (int $0x20), so it advances
   several times faster than wall time.  Sleep deadlines therefore use the TSC
   (apic_now_tsc) rather than this counter, which is kept only as a coarse
   activity count. */
static volatile uint64_t g_ticks;
static struct thread *sleep_head;  /* protected by rq_lock */

/* Threads killed while off-CPU, waiting to be freed by the next tick.  A thread
   that dies while running is handled by reap_pending[cpu] instead: it is still
   executing on its own stack, so it cannot be freed until it has switched off.
   Protected by rq_lock. */
static struct thread *zombie_head;

/* Unlink t from the sleep list if it is on it.  Returns true if it was — the
   caller then knows the thread was blocked in sched_sleep_ms and is therefore on
   no other queue, which is what makes it safe to free.  Caller holds rq_lock. */
static bool sleep_unlink_locked(struct thread *t)
{
	for (struct thread **sp = &sleep_head; *sp; sp = &(*sp)->sleep_next) {
		if (*sp == t) {
			*sp = t->sleep_next;
			t->sleep_next = NULL;
			return true;
		}
	}
	return false;
}

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

/* rq_lock is IRQ-safe: sched_tick takes it from the APIC timer ISR, so a thread
   preempted while holding it would deadlock the ISR on the same CPU. */
void sched_add(struct thread *t)
{
	uint64_t f = spinlock_acquire_irqsave(&rq_lock);
	rq_add(t);
	spinlock_release_irqrestore(&rq_lock, f);
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
	uint64_t f = spinlock_acquire_irqsave(&rq_lock);
	/* Only a BLOCKED thread may be made runnable, and this is the single place
	   that decides it.  Two cases the old unconditional store got wrong:
	   THREAD_DEAD is terminal, so a killed thread must never be resurrected by
	   a mutex_unlock or an exit notification that still holds a pointer to it;
	   and a thread already READY (woken by another source — a wait queue and a
	   sleep deadline can both fire) must not be enqueued a second time. */
	if (t->state == THREAD_BLOCKED) {
		sleep_unlink_locked(t);
		t->state = THREAD_READY;
		rq_add(t);
	}
	spinlock_release_irqrestore(&rq_lock, f);
}

/* Terminate a thread that may be blocked, sleeping, or running on another CPU.
   Taking rq_lock (rather than storing THREAD_DEAD from thread context, as
   process_kill used to) means this cannot race a state transition in sched_tick,
   and lets us pull a sleeper off the sleep list in the same breath — otherwise
   the deadline would later fire and hand the corpse back to the run queue. */
bool sched_kill(struct thread *t)
{
	if (!t) return true;
	uint64_t f = spinlock_acquire_irqsave(&rq_lock);
	if (t->state == THREAD_DEAD) {
		spinlock_release_irqrestore(&rq_lock, f);
		return true;
	}
	/* Never kill a thread that holds a kernel lock.  Stopping it dead left the
	   lock held forever — a victim killed inside the VFS wedged the filesystem for
	   every other process — and releasing the lock on its behalf would be worse,
	   publishing whatever half-finished state the operation was in.  Instead flag
	   it: the syscall runs to completion, releases everything, and the thread dies
	   on the way out (see syscall_dispatch). */
	if (t->locks_held > 0) {
		t->kill_pending = true;
		spinlock_release_irqrestore(&rq_lock, f);
		serial_write("sched: kill deferred to syscall return (holds a lock)\n");
		return false;
	}
	bool was_sleeping = sleep_unlink_locked(t);
	bool blocked      = (t->state == THREAD_BLOCKED);
	t->state = THREAD_DEAD;
	/* Free it here only if it was purely asleep, i.e. on the sleep list and on no
	   wait queue: nothing else then holds a pointer to it.  A thread linked in a
	   mutex or process wait queue (via wait_next) stays allocated and simply never
	   runs again — sched_wake refuses a DEAD thread, so whoever pops it later just
	   drops it.  Freeing it instead would leave that queue pointing into freed
	   memory.  The struct and its 64 KiB stack leak: a bounded, deliberate cost
	   next to the use-after-free the old code produced.  A RUNNING or READY thread
	   is reaped by sched_tick, which already handles both. */
	if (was_sleeping && !t->on_waitq && !t->on_cpu) {
		t->sleep_next = zombie_head;
		zombie_head   = t;
	} else if (blocked) {
		serial_write("sched: killed a thread blocked in a wait queue; "
		             "struct and stack leaked\n");
	}
	spinlock_release_irqrestore(&rq_lock, f);
	return true;
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
		/* The process struct is *not* touched here: t->process may already
		   have been freed by a parent's waitpid, and reading it to decide
		   orphan status was a use-after-free.  sys_exit and process_kill now
		   collect an orphan themselves, while the process is still live. */
		kfree(dead->kstack);
		kfree(dead);
	}

	/* Free one thread that was killed while off-CPU (see sched_kill).  One per
	   tick keeps this off the hot path and bounded; the list drains as ticks
	   pass.  Plain acquire: the ISR path already runs with IF clear. */
	{
		spinlock_acquire(&rq_lock);
		struct thread *z = zombie_head;
		if (z != NULL) {
			zombie_head   = z->sleep_next;
			z->sleep_next = NULL;
		}
		spinlock_release(&rq_lock);
		if (z != NULL) {
			if (z->is_user && z->cr3 != 0) {
				vmm_space_destroy(z->cr3);
				z->cr3 = 0;
			}
			kfree(z->kstack);
			kfree(z);
		}
	}

	struct thread *cur = cpu_current[cpu];
	cur->rsp = old_rsp;

	lapic_eoi();

	/* Plain acquire: we are on the ISR path, so IF is already clear and the
	   irqsave variant would only add a redundant pushfq/popfq to the hottest
	   path in the kernel.  Every thread-context taker uses the irqsave form. */
	spinlock_acquire(&rq_lock);

	/* Advance the activity counter and wake any sleeper whose deadline has passed
	   on the real clock. */
	g_ticks++;
	uint64_t now_tsc = apic_now_tsc();
	struct thread **sp = &sleep_head;
	while (*sp) {
		struct thread *st = *sp;
		if (st->sleep_deadline <= now_tsc) {
			*sp = st->sleep_next;
			st->sleep_next = NULL;
			/* Unlink unconditionally, but only a still-BLOCKED thread goes
			   back on the run queue.  A DEAD one was killed while sleeping
			   and must stay dead — re-queueing it used to resurrect a
			   process whose struct the parent's waitpid had already freed,
			   and it resumed user code through a dangling pointer.  A thread
			   already READY was woken by another source and is queued. */
			if (st->state == THREAD_BLOCKED) {
				st->state = THREAD_READY;
				rq_add(st);
			} else if (st->state == THREAD_DEAD && !st->on_cpu) {
				st->sleep_next = zombie_head;
				zombie_head    = st;
			}
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

	/* Never dispatch a corpse.  A thread killed while sitting READY in the run
	   queue is still linked here; dispatching it would iretq straight back into
	   a process that has already been torn down.  Drop it and take the next. */
	struct thread *next = rq_take();
	while (next != NULL && next->state == THREAD_DEAD) {
		if (!next->on_cpu) {
			next->sleep_next = zombie_head;
			zombie_head      = next;
		}
		next = rq_take();
	}
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

	uint64_t f = spinlock_acquire_irqsave(&rq_lock);
	/* Deadline on the monotonic clock.  This used to be `g_ticks + ms`, which
	   made every sleep expire early by however many times the scheduler happened
	   to be entered per millisecond — `sleep 30` returned after about 10 s. */
	t->sleep_deadline = apic_now_tsc() + ms * apic_tsc_per_ms();
	t->state          = THREAD_BLOCKED;
	t->sleep_next     = sleep_head;
	sleep_head        = t;
	/* Drop the lock but stay in the irqsave window: the timer must not preempt
	   between marking BLOCKED and yielding, and int $0x20 fires with IF clear.
	   The ISR's iretq restores IF=0, so restore the caller's flags after. */
	spinlock_release(&rq_lock);
	__asm__ volatile("int $0x20" ::: "memory");
	__asm__ volatile("push %0; popfq" :: "r"(f) : "memory", "cc");
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
