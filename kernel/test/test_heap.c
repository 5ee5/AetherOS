#include "test/ktest.h"

#include <stddef.h>
#include <stdint.h>

#include "mem/heap.h"

void test_heap_run(void)
{
	ktest_suite("heap");

	/* basic allocation returns non-NULL */
	void *a = kmalloc(64);
	KTEST_ASSERT(a != NULL);

	/* two allocations must not overlap */
	void *b = kmalloc(64);
	KTEST_ASSERT(b != NULL);
	KTEST_ASSERT((char *)b >= (char *)a + 64U);

	/* memory is writable */
	volatile uint64_t *pa = (volatile uint64_t *)a;
	volatile uint64_t *pb = (volatile uint64_t *)b;
	*pa = 0xaaaaaaaaaaaaaaULL;
	*pb = 0xbbbbbbbbbbbbbbULL;
	KTEST_ASSERT(*pa == 0xaaaaaaaaaaaaaaULL);
	KTEST_ASSERT(*pb == 0xbbbbbbbbbbbbbbULL);

	/* free then re-allocate the same size — must reuse (no leak) */
	kfree(b);
	void *c = kmalloc(64);
	KTEST_ASSERT(c != NULL);

	/* NULL free is a no-op */
	kfree(NULL);

	/* kmalloc(0) returns NULL */
	KTEST_ASSERT(kmalloc(0) == NULL);

	/* large allocation spanning multiple pages */
	void *big = kmalloc(8192);
	KTEST_ASSERT(big != NULL);
	volatile uint8_t *bp = (volatile uint8_t *)big;
	for (uint64_t i = 0; i < 8192U; ++i) {
		bp[i] = (uint8_t)(i & 0xff);
	}
	for (uint64_t i = 0; i < 8192U; ++i) {
		KTEST_ASSERT(bp[i] == (uint8_t)(i & 0xff));
	}

	kfree(big);
	kfree(c);
	kfree(a);

	/* after freeing everything, a fresh allocation should succeed */
	void *d = kmalloc(128);
	KTEST_ASSERT(d != NULL);
	kfree(d);
}
