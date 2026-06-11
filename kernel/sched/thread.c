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
	struct thread *t = (struct thread *)kmalloc(sizeof(struct thread));
	if (t == NULL) {
		panic("thread: kmalloc failed");
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
	t->on_cpu     = false;
	return t;
}

struct thread *thread_create(thread_fn_t fn, void *arg)
{
	struct thread *t = alloc_thread();

	t->kstack_size = THREAD_KSTACK_SIZE;
	t->kstack      = kmalloc(THREAD_KSTACK_SIZE);
	if (t->kstack == NULL) {
		panic("thread: stack kmalloc failed");
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
	struct thread *t = alloc_thread();
	t->state = THREAD_RUNNING;
	/* rsp will be filled in by the first sched_tick invocation. */
	return t;
}

struct thread *thread_create_idle(void)
{
	struct thread *t = alloc_thread();
	t->kstack_size = THREAD_KSTACK_SIZE;
	t->kstack      = kmalloc(THREAD_KSTACK_SIZE);
	if (t->kstack == NULL) {
		panic("thread: idle stack kmalloc failed");
	}
	t->kstack_top = (uint64_t)(uintptr_t)t->kstack + THREAD_KSTACK_SIZE;
	t->is_idle = true;
	t->state   = THREAD_RUNNING;
	/* rsp set on first preemption. */
	return t;
}

struct thread *thread_create_user(uint64_t entry_rip, uint64_t user_rsp, uint64_t cr3)
{
	struct thread *t = alloc_thread();

	t->kstack_size = THREAD_KSTACK_SIZE;
	t->kstack      = kmalloc(THREAD_KSTACK_SIZE);
	if (t->kstack == NULL) {
		panic("thread: user kstack kmalloc failed");
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

	sched_add(t);
	return t;
}

void thread_exit(void)
{
	__asm__ volatile("cli" ::: "memory");
	sched_current()->state = THREAD_DEAD;
	__asm__ volatile("int $0x20" ::: "memory");
	/* Never reached. */
	for (;;) {
		__asm__ volatile("hlt");
	}
}

void thread_yield(void)
{
	__asm__ volatile("int $0x20" ::: "memory");
}
