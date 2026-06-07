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
