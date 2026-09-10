#include "sched/thread.h"

#include <stddef.h>
#include <stdint.h>

#include "core/panic.h"
#include "mem/heap.h"
#include "sched/sched.h"

#define THREAD_KSTACK_SIZE 65536ULL

static uint32_t next_tid;

/*
 * Initial kernel stack frame layout (20 qwords, growing downward from
 * stack_top). Pushed order matches apic_timer_isr in sched_asm.asm:
 *   rax, rbx, rcx, rdx, rbp, rdi, rsi, r8..r15  (15 GP regs)
 *   + hardware iretq frame: RIP, CS, RFLAGS, RSP, SS
 *
 * After apic_timer_isr pops r15..rax and executes iretq, the CPU restores
 * RIP/CS/RFLAGS and RSP/SS from the hardware frame.
 */
#define FRAME_QWORDS 20   /* 15 GP + 5 hardware */

/* Indices within the 20-qword frame (from lowest address = index 0). */
#define IDX_R15  0
#define IDX_R14  1
#define IDX_R13  2
#define IDX_R12  3
#define IDX_R11  4
#define IDX_R10  5
#define IDX_R9   6
#define IDX_R8   7
#define IDX_RSI  8   /* fn */
#define IDX_RDI  9   /* arg */
#define IDX_RBP  10
#define IDX_RDX  11
#define IDX_RCX  12
#define IDX_RBX  13
#define IDX_RAX  14
#define IDX_RIP  15
#define IDX_CS   16
#define IDX_RFLAGS 17
#define IDX_RSP  18   /* restored RSP after iretq */
#define IDX_SS   19

static struct thread *alloc_thread(void)
{
	/* Returns NULL rather than panicking: a user process can drive this from a
	   spawn loop, and halting the machine on a failed allocation turned an
	   unprivileged out-of-memory condition into a denial of service.  Every
	   caller propagates the NULL up to the syscall, which fails with -ENOMEM. */
	struct thread *t = (struct thread *)kmalloc(sizeof(struct thread));
	if (t == NULL) {
		return NULL;
	}
	t->rsp        = 0;
	t->kstack     = NULL;
	t->kstack_size = 0;
	t->kstack_top  = 0;
	t->state      = THREAD_READY;
	t->tid        = next_tid++;
	t->is_idle    = false;
	t->is_user    = false;
	t->cr3        = 0;
	t->run_next   = NULL;
	t->wait_next  = NULL;
	t->mutex_next = NULL;
	t->on_waitq   = false;
	t->locks_held = 0;
	t->kill_pending = false;
	t->on_cpu     = false;
	/* kmalloc does not zero, so these must be set explicitly.  `process` in
	   particular is read by sched_current_process() on every kernel thread;
	   left uninitialized it is a heap-garbage pointer that callers treat as a
	   live process (permission checks and vfs_resolve both dereference it). */
	t->process        = NULL;
	t->sleep_next     = NULL;
	t->sleep_deadline = 0;

	/* Point fpu at the first 16-byte-aligned byte of the slack buffer; see the
	   note in struct thread for why this cannot rely on kmalloc's alignment. */
	t->fpu = (uint8_t *)(((uintptr_t)t->fpu_area + 15U) & ~(uintptr_t)15U);

	/* A valid "freshly initialised FPU" FXSAVE image: all zero except the x87
	   control word and MXCSR, which must hold their architectural defaults.
	   FXRSTOR #GPs if MXCSR has reserved bits set, so this cannot just be
	   zeroed.  (MXCSR_MASK is ignored on restore, so leaving it 0 is fine.) */
	for (uint32_t i = 0; i < 512U; ++i) t->fpu[i] = 0;
	t->fpu[0] = 0x7f; t->fpu[1] = 0x03;          /* FCW   = 0x037f */
	t->fpu[24] = 0x80; t->fpu[25] = 0x1f;        /* MXCSR = 0x1f80 */
	return t;
}

struct thread *thread_create(thread_fn_t fn, void *arg)
{
	struct thread *t = alloc_thread();
	if (t == NULL) {
		return NULL;
	}

	t->kstack_size = THREAD_KSTACK_SIZE;
	t->kstack      = kmalloc(THREAD_KSTACK_SIZE);
	if (t->kstack == NULL) {
		kfree(t);
		return NULL;
	}
	t->kstack_top = (uint64_t)(uintptr_t)t->kstack + THREAD_KSTACK_SIZE;

	/* Set up the initial stack frame at the top of the stack. */
	uint64_t *stack_top =
		(uint64_t *)((char *)t->kstack + THREAD_KSTACK_SIZE);
	uint64_t *frame = stack_top - FRAME_QWORDS;

	for (int i = 0; i < FRAME_QWORDS; ++i) {
		frame[i] = 0;
	}

	frame[IDX_RSI]   = (uint64_t)(uintptr_t)fn;
	frame[IDX_RDI]   = (uint64_t)(uintptr_t)arg;
	frame[IDX_RIP]   = (uint64_t)(uintptr_t)thread_run_trampoline;
	frame[IDX_CS]    = 0x08ULL;              /* kernel code selector */
	frame[IDX_RFLAGS] = 0x202ULL;            /* IF=1, reserved bit 1 */
	frame[IDX_RSP]   = (uint64_t)(uintptr_t)stack_top;
	frame[IDX_SS]    = 0x10ULL;              /* kernel data selector */

	t->rsp = (uint64_t)(uintptr_t)frame;

	sched_add(t);
	return t;
}

struct thread *thread_create_current(void)
{
	/* Boot-time only (registering the running CPU as a thread), and there is no
	   way to continue without it. */
	struct thread *t = alloc_thread();
	if (t == NULL) {
		panic("thread: current-thread kmalloc failed");
	}
	t->state = THREAD_RUNNING;
	/* rsp will be filled in by the first sched_tick invocation. */
	return t;
}

struct thread *thread_create_idle(void)
{
	/* Boot-time only, and the scheduler cannot run without an idle thread, so
	   failing here really is unrecoverable — unlike the user-driven paths. */
	struct thread *t = alloc_thread();
	if (t == NULL) {
		panic("thread: idle thread kmalloc failed");
	}
	t->kstack_size = THREAD_KSTACK_SIZE;
	t->kstack      = kmalloc(THREAD_KSTACK_SIZE);
	if (t->kstack == NULL) {
		panic("thread: idle stack kmalloc failed");
	}
	t->kstack_top = (uint64_t)(uintptr_t)t->kstack + THREAD_KSTACK_SIZE;
	t->is_idle = true;
	t->state   = THREAD_RUNNING;

	/* Give the idle thread a real initial frame instead of leaving rsp at 0 to
	   be "set on first preemption".  That held only while an idle thread could
	   never be *dispatched* before it had first been switched away from: the
	   run queue always contained the current thread, so rq_take never came back
	   empty.  It does now -- every queued thread can be momentarily unavailable
	   while it finishes switching off another CPU -- and dispatching an idle
	   thread with rsp == 0 makes the scheduler stub load RSP=0 and double-fault.
	   Entering here simply runs the idle loop, which is what the CPU would be
	   doing anyway. */
	uint64_t *stack_top =
		(uint64_t *)((char *)t->kstack + THREAD_KSTACK_SIZE);
	uint64_t *frame = stack_top - FRAME_QWORDS;

	for (int i = 0; i < FRAME_QWORDS; ++i) {
		frame[i] = 0;
	}

	frame[IDX_RSI]    = (uint64_t)(uintptr_t)sched_idle_loop;  /* fn  */
	frame[IDX_RDI]    = 0;                                     /* arg */
	frame[IDX_RIP]    = (uint64_t)(uintptr_t)thread_run_trampoline;
	frame[IDX_CS]     = 0x08ULL;
	frame[IDX_RFLAGS] = 0x202ULL;            /* IF=1 */
	frame[IDX_RSP]    = (uint64_t)(uintptr_t)stack_top;
	frame[IDX_SS]     = 0x10ULL;

	t->rsp = (uint64_t)(uintptr_t)frame;
	return t;
}

struct thread *thread_create_user(uint64_t entry_rip, uint64_t user_rsp, uint64_t cr3)
{
	struct thread *t = alloc_thread();
	if (t == NULL) {
		return NULL;
	}

	t->kstack_size = THREAD_KSTACK_SIZE;
	t->kstack      = kmalloc(THREAD_KSTACK_SIZE);
	if (t->kstack == NULL) {
		kfree(t);
		return NULL;
	}
	t->kstack_top = (uint64_t)(uintptr_t)t->kstack + THREAD_KSTACK_SIZE;
	t->is_user    = true;
	t->cr3        = cr3;

	/* Same 20-qword iretq frame as kernel threads, but with user selectors. */
	uint64_t *stack_top = (uint64_t *)(uintptr_t)t->kstack_top;
	uint64_t *frame     = stack_top - FRAME_QWORDS;

	for (int i = 0; i < FRAME_QWORDS; ++i) {
		frame[i] = 0;
	}

	frame[IDX_RIP]    = entry_rip;
	frame[IDX_CS]     = 0x33ULL;   /* GDT_USER_CODE_64, RPL=3 */
	frame[IDX_RFLAGS] = 0x202ULL;  /* IF=1 */
	frame[IDX_RSP]    = user_rsp;
	frame[IDX_SS]     = 0x2bULL;   /* GDT_USER_DATA, RPL=3 */

	t->rsp = (uint64_t)(uintptr_t)frame;

	/* NOT added to the run queue here.  The owning process must finish
	   initializing credentials/fds before making the thread runnable via
	   process_start(); otherwise the child can be scheduled on another CPU
	   and run with stale (root) credentials and no inherited fds. */
	return t;
}

void thread_exit(void)
{
	__asm__ volatile("cli" ::: "memory");
	sched_current()->state = THREAD_DEAD;
	SCHED_YIELD();
	/* Never reached. */
	for (;;) {
		__asm__ volatile("hlt");
	}
}

void thread_yield(void)
{
	SCHED_YIELD();
}
