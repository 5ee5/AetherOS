#ifndef KERNEL_SCHED_THREAD_H
#define KERNEL_SCHED_THREAD_H

#include <stdbool.h>
#include <stdint.h>

typedef void (*thread_fn_t)(void *arg);

typedef enum {
	THREAD_READY   = 0,
	THREAD_RUNNING = 1,
	THREAD_BLOCKED = 2,
	THREAD_DEAD    = 3,
} thread_state_t;

struct thread {
	uint64_t       rsp;        /* saved kernel stack pointer */
	void          *kstack;     /* bottom of stack allocation (NULL for BSP) */
	uint64_t       kstack_size;
	uint64_t       kstack_top; /* = (uint64_t)kstack + kstack_size; for tss rsp0 */
	thread_state_t state;
	uint32_t       tid;
	bool           is_idle;    /* never add to run queue */
	bool           is_user;    /* ring-3 thread; cr3 is valid */
	volatile bool  on_cpu;     /* executing on a CPU now; gates cross-CPU wakeups */
	uint64_t       cr3;        /* PML4 phys for user thread (0 = kernel) */
	struct thread *run_next;     /* run queue linkage */
	struct thread *wait_next;    /* wait/blocked queue linkage */
	struct thread *sleep_next;   /* sleep queue linkage */
	uint64_t       sleep_deadline; /* tick at which to wake (0 = not sleeping) */
	void          *process;      /* owning struct process, or NULL */
};

/* Create a new kernel thread. Added to run queue automatically. */
struct thread *thread_create(thread_fn_t fn, void *arg);

/* Create a user-mode thread. Added to run queue automatically.
   entry_rip and user_rsp are the ring-3 RIP/RSP; cr3 is the process PML4 phys. */
struct thread *thread_create_user(uint64_t entry_rip, uint64_t user_rsp, uint64_t cr3);

/* Register the current execution context as a scheduler thread (BSP). */
struct thread *thread_create_current(void);

/* Create an idle thread (is_idle=true, NOT added to run queue). */
struct thread *thread_create_idle(void);

void thread_exit(void);
void thread_yield(void);

/* Assembly trampoline that calls fn(arg) then thread_exit. */
void thread_run_trampoline(void);

#endif
