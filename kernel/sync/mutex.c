#include "sync/mutex.h"

#include <stdbool.h>
#include <stddef.h>

#include "sched/sched.h"
#include "sched/thread.h"
#include "sync/spinlock.h"

void mutex_lock(mutex_t *m)
{
	spinlock_acquire(&m->lock);
	while (m->locked) {
		struct thread *cur = sched_current();
		/* Add to wait list and mark blocked.  mutex_next, not wait_next: the
		   latter belongs to the process wait queue, and one shared pointer
		   between the two lists truncates whichever was linked first. */
		/* Same THREAD_DEAD hazard as sched_sleep_ms: a thread killed while
		   entering mutex_lock must not have DEAD overwritten with BLOCKED, or
		   a later unlock resurrects the corpse onto the run queue. */
		if (cur->state == THREAD_DEAD) {
			spinlock_release(&m->lock);
			SCHED_YIELD();
			return;
		}
		cur->mutex_next = m->waiters;
		m->waiters      = cur;
		cur->state      = THREAD_BLOCKED;
		spinlock_release(&m->lock);

		/* Disable interrupts so the timer cannot preempt between here
		   and int $SCHED_YIELD_VECTOR, preventing a double-enqueue race where a
		   concurrent wakeup sets state=READY before we sleep. */
		__asm__ volatile("cli" ::: "memory");
		SCHED_YIELD();
		/* iretq restored RFLAGS with IF=0 (we called cli before int).
		   Re-enable interrupts now that we're awake. */
		__asm__ volatile("sti" ::: "memory");

		spinlock_acquire(&m->lock);
	}
	m->locked = true;
	m->owner  = sched_current();
	if (m->owner) m->owner->locks_held++;
	spinlock_release(&m->lock);
}

void mutex_unlock(mutex_t *m)
{
	spinlock_acquire(&m->lock);
	m->locked = false;
	if (m->owner && m->owner->locks_held > 0) m->owner->locks_held--;
	m->owner  = NULL;
	/* Hand the lock on to a waiter that will actually re-test it.
	 *
	 * Popping exactly one waiter and calling sched_wake was not enough:
	 * sched_wake only promotes a THREAD_BLOCKED thread and silently does
	 * nothing otherwise, so a wakeup aimed at a waiter that had already been
	 * made runnable, or had died, was simply dropped -- leaving the mutex free
	 * with the remaining waiters asleep and nothing left to wake them.  Keep
	 * going until one is genuinely made runnable, or the list is empty.
	 *
	 * Each woken thread re-tests `while (m->locked)` and re-queues itself if it
	 * loses the race, so waking more than strictly necessary is safe. */
	while (m->waiters != NULL) {
		struct thread *woken = m->waiters;
		m->waiters        = woken->mutex_next;
		woken->mutex_next = NULL;
		spinlock_release(&m->lock);

		bool made_runnable = sched_wake(woken);

		if (made_runnable) {
			return;
		}
		/* That one could not take the wakeup; try the next. */
		spinlock_acquire(&m->lock);
	}
	spinlock_release(&m->lock);
}

bool mutex_try_lock(mutex_t *m)
{
	spinlock_acquire(&m->lock);
	if (m->locked) {
		spinlock_release(&m->lock);
		return false;
	}
	m->locked = true;
	m->owner  = sched_current();
	if (m->owner) m->owner->locks_held++;
	spinlock_release(&m->lock);
	return true;
}
