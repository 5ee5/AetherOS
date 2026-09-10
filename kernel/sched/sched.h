#ifndef KERNEL_SCHED_SCHED_H
#define KERNEL_SCHED_SCHED_H

#include <stdbool.h>
#include <stdint.h>

struct thread;
struct process;

void sched_init(void);
void sched_add(struct thread *t);
struct thread  *sched_current(void);
struct process *sched_current_process(void);

/* Wake a blocked thread: mark READY and re-enqueue.  Ignores a thread that is
   not BLOCKED, so a DEAD thread is never resurrected and one already runnable is
   never enqueued twice. */
/* Make a blocked thread runnable.  Returns true if it actually did: only a
   THREAD_BLOCKED thread is promoted, so a wakeup aimed at a thread in any other
   state (already runnable, or dead) is a no-op the caller may need to notice. */
bool sched_wake(struct thread *t);

/* Terminate a thread wherever it is: running, READY, sleeping, or blocked.
   THREAD_DEAD is terminal — the thread never executes again.

   Returns true if the thread is dead on return.  Returns false when the kill had
   to be deferred because the thread holds a kernel lock: it keeps running until
   the current syscall returns and then terminates itself, so the caller must not
   tear down anything the thread is still using. */
bool sched_kill(struct thread *t);

/* Vector for a voluntary reschedule, distinct from the APIC timer's 0x20 (and the
   PS/2 keyboard's 0x21).  The handler behind it neither advances the tick count nor
   signals end-of-interrupt, because nothing was actually delivered.  Yield through
   SCHED_YIELD() rather than writing the vector out by hand; the literal in the asm
   below must match this value. */
#define SCHED_YIELD_VECTOR 0x22
#define SCHED_YIELD() __asm__ volatile("int $0x22" ::: "memory")

/* Timer interrupts taken since boot, summed across CPUs.  Not a clock — use
   apic_now_tsc() for elapsed time.  Exposed so a test can check that voluntary
   yields do not inflate it. */
uint64_t sched_ticks(void);

/* Called from the timer ISR (assembly).
   old_rsp: RSP after saving all GP registers onto the current stack.
   Returns the RSP to restore (may be a different thread's stack). */
uint64_t sched_tick(uint64_t old_rsp);

/* Called from the voluntary-yield stub (assembly), same contract as sched_tick. */
uint64_t sched_yield_tick(uint64_t old_rsp);

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
