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

/* ---- Killing a sleeping thread must be final --------------------------- */

static volatile uint32_t sleeper_resumed;
static volatile uint32_t sleeper_started;

static void sleeper_victim(void *arg)
{
	(void)arg;
	sleeper_started = 1U;
	sched_sleep_ms(40);
	/* Unreachable: the test kills this thread while it is on the sleep list.
	   The sleeper loop in sched_tick used to set THREAD_READY unconditionally
	   when the deadline expired, which handed the corpse back to the run queue
	   and resumed it here — in a process whose struct the parent's waitpid had
	   already freed. */
	__sync_add_and_fetch(&sleeper_resumed, 1U);
	thread_exit();
}

/* ---- A kill must not strand a held lock -------------------------------- */

static volatile uint32_t defer_ready;
static volatile uint32_t defer_release;
static volatile uint32_t defer_done;
static mutex_t defer_mutex = MUTEX_INIT;

static void defer_victim(void *arg)
{
	(void)arg;
	mutex_lock(&defer_mutex);
	defer_ready = 1U;
	while (!defer_release) {
		thread_yield();
	}
	mutex_unlock(&defer_mutex);
	defer_done = 1U;
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

	/* --- THREAD_DEAD is terminal, even for a sleeper --- */
	sleeper_resumed = 0;
	sleeper_started = 0;

	__asm__ volatile("sti" ::: "memory");
	struct thread *victim = thread_create(sleeper_victim, NULL);
	KTEST_ASSERT(victim != NULL);
	while (!sleeper_started) {
		thread_yield();
	}
	/* Let it reach sched_sleep_ms and land on the sleep list. */
	for (uint32_t i = 0; i < 8U; ++i) {
		thread_yield();
	}
	sched_kill(victim);
	/* Sleep well past the victim's 40 ms deadline: if the kill were not final,
	   the expiry would run its remaining code and bump the counter. */
	sched_sleep_ms(200);
	__asm__ volatile("cli" ::: "memory");
	KTEST_ASSERT(sleeper_resumed == 0U);

	/* --- Killing a lock holder is deferred, not immediate ---
	   Stopping a thread dead while it holds a mutex left that mutex locked
	   forever, and a victim killed inside the VFS wedged the filesystem for every
	   other process.  sched_kill must refuse, letting the thread reach a point
	   where it holds nothing. */
	defer_ready = defer_release = defer_done = 0;

	__asm__ volatile("sti" ::: "memory");
	struct thread *holder = thread_create(defer_victim, NULL);
	KTEST_ASSERT(holder != NULL);
	while (!defer_ready) {
		thread_yield();
	}
	/* The victim is holding defer_mutex at this point. */
	KTEST_ASSERT(sched_kill(holder) == false);   /* deferred */
	KTEST_ASSERT(holder->kill_pending);
	KTEST_ASSERT(holder->state != THREAD_DEAD);  /* still alive to unlock */

	defer_release = 1U;
	while (!defer_done) {
		thread_yield();
	}
	__asm__ volatile("cli" ::: "memory");
	/* It released the mutex on its way out, so the lock is usable again — this is
	   the assertion the filesystem wedge would have failed. */
	KTEST_ASSERT(mutex_try_lock(&defer_mutex));
	mutex_unlock(&defer_mutex);
}
