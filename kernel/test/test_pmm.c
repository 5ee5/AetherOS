#include "test/ktest.h"

#include <stdint.h>

#include "mem/pmm.h"

void test_pmm_run(void)
{
	ktest_suite("pmm");

	uint64_t before = pmm_free_pages();

	/* basic alloc/free round-trip */
	uint64_t page = pmm_alloc_page();
	KTEST_ASSERT(page != PMM_ALLOC_FAILED);
	KTEST_ASSERT(pmm_address_in_range(page));
	KTEST_ASSERT(pmm_free_pages() == before - 1U);
	pmm_free_page(page);
	KTEST_ASSERT(pmm_free_pages() == before);

	/* multiple allocs produce distinct, in-range addresses */
	uint64_t pages[16];
	for (uint64_t i = 0; i < 16U; ++i) {
		pages[i] = pmm_alloc_page();
		KTEST_ASSERT(pages[i] != PMM_ALLOC_FAILED);
		KTEST_ASSERT(pmm_address_in_range(pages[i]));
	}
	for (uint64_t i = 0; i < 16U; ++i) {
		for (uint64_t j = i + 1U; j < 16U; ++j) {
			KTEST_ASSERT(pages[i] != pages[j]);
		}
	}
	for (uint64_t i = 0; i < 16U; ++i) {
		pmm_free_page(pages[i]);
	}
	KTEST_ASSERT(pmm_free_pages() == before);

	/* address_in_range: known-good and known-bad */
	KTEST_ASSERT(pmm_address_in_range(0x1000U));
	KTEST_ASSERT(!pmm_address_in_range(UINT64_MAX));
}
