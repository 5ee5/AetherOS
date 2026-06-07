#include "test/ktest.h"

#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/io.h"
#include "sched/sched.h"
#include "sched/thread.h"
#include "sync/mutex.h"
#include "sync/spinlock.h"

/* ---- Basic thread completion test ------------------------------------- */

#define NUM_THREADS 4

static volatile uint32_t completion_count;

static void counter_thread(void *arg)
{
	(void)arg;
	__sync_add_and_fetch(&completion_count, 1U);
	thread_exit();
}

/* ---- Mutex-protected shared counter test ------------------------------ */

static volatile uint32_t mutex_counter;
static mutex_t test_mutex = MUTEX_INIT;

static void mutex_thread(void *arg)
{
	(void)arg;
	for (uint32_t i = 0; i < 10U; ++i) {
		mutex_lock(&test_mutex);
		uint32_t v = mutex_counter;
		thread_yield();   /* force interleaving */
		mutex_counter = v + 1U;
		mutex_unlock(&test_mutex);
	}
	thread_exit();
}

/* ---- Yield test: threads should interleave ---------------------------- */

static volatile uint32_t yield_counter;

static void yield_thread(void *arg)
{
	uint32_t iters = (uint32_t)(uintptr_t)arg;
	for (uint32_t i = 0; i < iters; ++i) {
		__sync_add_and_fetch(&yield_counter, 1U);
		thread_yield();
	}
	thread_exit();
}

/* ---- Test runner ------------------------------------------------------- */

void test_sched_run(void)
{
	ktest_suite("sched");

	/* --- Thread completion --- */
	completion_count = 0;
	for (uint32_t i = 0; i < NUM_THREADS; ++i) {
		thread_create(counter_thread, NULL);
	}

	/* Enable interrupts so the APIC timer can drive scheduling. */
	__asm__ volatile("sti" ::: "memory");

	/* Spin until all threads have exited. */
	while (completion_count < NUM_THREADS) {
		__asm__ volatile("pause");
	}

	__asm__ volatile("cli" ::: "memory");
	KTEST_ASSERT(completion_count == NUM_THREADS);

	/* --- Mutex-protected counter --- */
	mutex_counter = 0;

	__asm__ volatile("sti" ::: "memory");
	thread_create(mutex_thread, NULL);
	thread_create(mutex_thread, NULL);

	/* Yield this thread so the mutex threads can run. */
	while (mutex_counter < 20U) {
		thread_yield();
	}
	__asm__ volatile("cli" ::: "memory");
	KTEST_ASSERT(mutex_counter == 20U);

	/* --- Yield interleaving --- */
	yield_counter = 0;

	__asm__ volatile("sti" ::: "memory");
	thread_create(yield_thread, (void *)(uintptr_t)5U);
	thread_create(yield_thread, (void *)(uintptr_t)5U);

	while (yield_counter < 10U) {
		thread_yield();
	}
	__asm__ volatile("cli" ::: "memory");
	KTEST_ASSERT(yield_counter == 10U);
}
