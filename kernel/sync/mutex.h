#ifndef KERNEL_SYNC_MUTEX_H
#define KERNEL_SYNC_MUTEX_H

#include <stdbool.h>

#include "sync/spinlock.h"

struct thread;

typedef struct {
	spinlock_t     lock;
	bool           locked;
	struct thread *owner;
	struct thread *waiters;  /* linked via wait_next */
} mutex_t;

#define MUTEX_INIT { .lock = SPINLOCK_INIT, .locked = false, \
                     .owner = NULL, .waiters = NULL }

void mutex_lock(mutex_t *m);
void mutex_unlock(mutex_t *m);
bool mutex_try_lock(mutex_t *m);

#endif
