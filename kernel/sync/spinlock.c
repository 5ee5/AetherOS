#include "sync/spinlock.h"

#include <stdbool.h>
#include <stdint.h>

static uint32_t xchg32(volatile uint32_t *addr, uint32_t val)
{
	uint32_t result;
	__asm__ volatile("xchg %0, %1"
		: "=r"(result), "+m"(*addr)
		: "0"(val)
		: "memory");
	return result;
}

void spinlock_acquire(spinlock_t *lock)
{
	while (xchg32(&lock->locked, 1U) != 0U) {
		__asm__ volatile("pause");
	}
}

void spinlock_release(spinlock_t *lock)
{
	__asm__ volatile("" ::: "memory");
	lock->locked = 0;
}

bool spinlock_try_acquire(spinlock_t *lock)
{
	return xchg32(&lock->locked, 1U) == 0U;
}

uint64_t spinlock_acquire_irqsave(spinlock_t *lock)
{
	uint64_t flags;
	__asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
	while (xchg32(&lock->locked, 1U) != 0U) {
		__asm__ volatile("pause");
	}
	return flags;
}

void spinlock_release_irqrestore(spinlock_t *lock, uint64_t flags)
{
	__asm__ volatile("" ::: "memory");
	lock->locked = 0;
	/* Restore the saved RFLAGS (re-enables IF only if it was set before). */
	__asm__ volatile("push %0; popfq" :: "r"(flags) : "memory", "cc");
}
