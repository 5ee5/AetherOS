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
		/* Add to wait list and mark blocked. */
		cur->wait_next = m->waiters;
		m->waiters     = cur;
		cur->state     = THREAD_BLOCKED;
		spinlock_release(&m->lock);

		/* Disable interrupts so the timer cannot preempt between here
		   and int $0x20, preventing a double-enqueue race where a
		   concurrent wakeup sets state=READY before we sleep. */
		__asm__ volatile("cli" ::: "memory");
		__asm__ volatile("int $0x20" ::: "memory");
		/* iretq restored RFLAGS with IF=0 (we called cli before int).
		   Re-enable interrupts now that we're awake. */
		__asm__ volatile("sti" ::: "memory");

		spinlock_acquire(&m->lock);
	}
	m->locked = true;
	m->owner  = sched_current();
	spinlock_release(&m->lock);
}

void mutex_unlock(mutex_t *m)
{
	spinlock_acquire(&m->lock);
	m->locked = false;
	m->owner  = NULL;
	if (m->waiters != NULL) {
		struct thread *woken = m->waiters;
		m->waiters = woken->wait_next;
		woken->wait_next = NULL;
		spinlock_release(&m->lock);
		sched_wake(woken);
	} else {
		spinlock_release(&m->lock);
	}
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
	spinlock_release(&m->lock);
	return true;
}
