#include "test/ktest.h"

#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/apic.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/io.h"
#include "arch/x86_64/smp.h"
#include "core/serial.h"
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

/* ---- Threads must actually run on more than one CPU ----------------------- */

#define SMP_PROBE_THREADS 4
#define SMP_PROBE_ITERS   64

static volatile uint32_t s_smp_seen[MAX_CPUS];
static volatile uint32_t s_smp_distinct;
static volatile uint32_t s_smp_finished;

static void smp_probe_thread(void *arg)
{
	(void)arg;
	/* Run long enough to be preempted and possibly migrate, recording every CPU
	   this thread is observed on.  Stop as soon as a second CPU has been seen by
	   anyone: the whole point is proving work reaches more than one core, and
	   the USB boot path is slow enough that spinning past that wastes most of
	   the smoke test's time budget. */
	for (int i = 0; i < SMP_PROBE_ITERS; ++i) {
		uint32_t id = lapic_id();
		if (id < MAX_CPUS && s_smp_seen[id] == 0U) {
			s_smp_seen[id] = 1U;
			__atomic_fetch_add(&s_smp_distinct, 1U, __ATOMIC_SEQ_CST);
		}
		if (i >= 8 && __atomic_load_n(&s_smp_distinct, __ATOMIC_SEQ_CST) > 1U) {
			break;
		}
		thread_yield();
	}
	__atomic_fetch_add(&s_smp_finished, 1U, __ATOMIC_SEQ_CST);
}

/* Every AP used to arm its timer with a period read from its own (zeroed)
   LAPIC register, which disabled the timer outright, so no AP ever ran a
   thread and the second core was decorative.  Assert that work really does
   land on more than one CPU when more than one is online. */
static void test_smp_threads_run_on_all_cpus(void)
{
	uint32_t cpus = smp_cpus_online();

	/* Preemption (and therefore any migration to an AP) needs interrupts. */
	__asm__ volatile("sti" ::: "memory");

	for (uint32_t i = 0; i < MAX_CPUS; ++i) {
		s_smp_seen[i] = 0U;
	}
	s_smp_distinct = 0U;
	s_smp_finished = 0U;

	for (int i = 0; i < SMP_PROBE_THREADS; ++i) {
		KTEST_ASSERT(thread_create(smp_probe_thread, NULL) != NULL);
	}

	for (int spins = 0; spins < 200000 &&
	     __atomic_load_n(&s_smp_finished, __ATOMIC_SEQ_CST) < SMP_PROBE_THREADS;
	     ++spins) {
		thread_yield();
	}
	KTEST_ASSERT(__atomic_load_n(&s_smp_finished, __ATOMIC_SEQ_CST) ==
		     SMP_PROBE_THREADS);

	uint32_t distinct = 0;
	for (uint32_t i = 0; i < MAX_CPUS; ++i) {
		if (s_smp_seen[i]) {
			distinct++;
		}
	}

	serial_write("ktest: threads observed on ");
	serial_write_dec(distinct);
	serial_write(" of ");
	serial_write_dec(cpus);
	serial_write(" cpus\n");

	/* With a single CPU online, one is the correct answer. */
	if (cpus > 1U) {
		KTEST_ASSERT(distinct > 1U);
	} else {
		KTEST_ASSERT(distinct == 1U);
	}

	__asm__ volatile("cli" ::: "memory");
}

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

	/* --- A voluntary yield is not a timer interrupt ---
	   Both used to arrive on vector 0x20, so g_ticks counted yields as ticks (and
	   every yield sent the LAPIC an end-of-interrupt for something it had never
	   delivered).  Yields now come in on their own vector: a few thousand of them
	   must barely move the tick count, where before each one added exactly 1. */
	__asm__ volatile("sti" ::: "memory");
	uint64_t ticks_before = sched_ticks();
	for (uint32_t i = 0; i < 2000U; ++i) {
		thread_yield();
	}
	uint64_t ticks_delta = sched_ticks() - ticks_before;
	__asm__ volatile("cli" ::: "memory");
	/* Real interrupts in that window are on the order of tens per CPU at ~1 kHz;
	   the bound is loose, but it fails hard (delta >= 2000) if yields are counted. */
	KTEST_ASSERT(ticks_delta < 500U);

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

	test_smp_threads_run_on_all_cpus();
}
