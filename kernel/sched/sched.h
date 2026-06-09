#ifndef KERNEL_SCHED_SCHED_H
#define KERNEL_SCHED_SCHED_H

#include <stdint.h>

struct thread;
struct process;

void sched_init(void);
void sched_add(struct thread *t);
struct thread  *sched_current(void);
struct process *sched_current_process(void);

/* Wake a blocked thread: mark READY and re-enqueue. */
void sched_wake(struct thread *t);

/* Called from the timer ISR (assembly).
   old_rsp: RSP after saving all GP registers onto the current stack.
   Returns the RSP to restore (may be a different thread's stack). */
uint64_t sched_tick(uint64_t old_rsp);

/* Called by BSP (from smp_init) to allocate the idle thread for an AP
   before the AP is started. Must be called with interrupts disabled. */
void sched_ap_prepare(uint32_t cpu_id);

/* Called by each AP after its LAPIC is initialized. */
void sched_ap_enter(void);

/* Assembly idle loop (defined in sched_asm.asm). */
void sched_idle_loop(void);

/* Block the current thread for approximately ms milliseconds (~1ms/tick). */
void sched_sleep_ms(uint64_t ms);

#endif
