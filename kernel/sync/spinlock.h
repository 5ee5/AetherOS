#ifndef KERNEL_SYNC_SPINLOCK_H
#define KERNEL_SYNC_SPINLOCK_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	volatile uint32_t locked;
} spinlock_t;

#define SPINLOCK_INIT { .locked = 0 }

void spinlock_acquire(spinlock_t *lock);
void spinlock_release(spinlock_t *lock);
bool spinlock_try_acquire(spinlock_t *lock);

/* IRQ-safe acquire/release. Returns the previous RFLAGS so the caller can
   restore the prior interrupt state. Required for locks that protect state
   reachable from interrupt context (e.g. the heap, freed from the timer ISR's
   deferred reaper) — a plain spinlock would self-deadlock if the ISR fired
   while a thread held the lock on the same CPU. */
uint64_t spinlock_acquire_irqsave(spinlock_t *lock);
void spinlock_release_irqrestore(spinlock_t *lock, uint64_t flags);

#endif
