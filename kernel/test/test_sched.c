#include "test/ktest.h"

#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/io.h"
#include "mem/heap.h"
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

/* ---- Heap concurrency stress (Phase 2 IRQ-safe heap lock) -------------- */

#define HEAP_STRESS_THREADS 4
#define HEAP_STRESS_ITERS   200

static volatile uint32_t heap_stress_done;
static volatile uint32_t heap_stress_bad;

static void heap_stress_thread(void *arg)
{
	(void)arg;
	for (uint32_t i = 0; i < HEAP_STRESS_ITERS; ++i) {
		uint8_t *a = (uint8_t *)kmalloc(64);
		uint8_t *b = (uint8_t *)kmalloc(256);
		if (!a || !b) {
			__sync_add_and_fetch(&heap_stress_bad, 1U);
		} else {
			a[0] = 0xAA; a[63] = 0x55;   /* touch to catch overlap */
		}
		kfree(a);
		kfree(b);
		thread_yield();
	}
	__sync_add_and_fetch(&heap_stress_done, 1U);
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

	/* --- Heap concurrency stress ---
	   Several threads hammer kmalloc/kfree while the timer ISR's deferred
	   reaper also calls kfree. If the heap lock weren't IRQ-safe, a thread
	   preempted mid-allocation would deadlock the ISR on the same CPU and
	   this loop would hang (smoke test timeout). */
	heap_stress_done = 0;
	heap_stress_bad  = 0;

	__asm__ volatile("sti" ::: "memory");
	for (uint32_t i = 0; i < HEAP_STRESS_THREADS; ++i) {
		thread_create(heap_stress_thread, NULL);
	}
	while (heap_stress_done < HEAP_STRESS_THREADS) {
		thread_yield();
	}
	__asm__ volatile("cli" ::: "memory");
	KTEST_ASSERT(heap_stress_done == HEAP_STRESS_THREADS);
	KTEST_ASSERT(heap_stress_bad == 0U);
}
