#include "test/ktest.h"

#include <stdint.h>

#include "mem/pmm.h"
#include "mem/vmm.h"

void kernel_main(const void *);  /* forward decl for address-of */

/* First virtual page above the 64 GiB identity map — intermediate page
   tables are absent so ensure_table will allocate them. */
#define TEST_VIRT  0x0000001000000000ULL
#define TEST_VIRT2 0x0000001000001000ULL

void test_vmm_run(void)
{
	ktest_suite("vmm");

	uint64_t phys = 0;

	/* a mapped kernel address must translate successfully */
	KTEST_ASSERT(vmm_virt_to_phys((uint64_t)(uintptr_t)kernel_main, &phys));
	KTEST_ASSERT(phys != 0U);
	KTEST_ASSERT(phys < (64ULL * 1024ULL * 1024ULL * 1024ULL));

	/* the direct-map base must translate to physical address 0 */
	uint64_t dm_phys = 1U;
	KTEST_ASSERT(vmm_virt_to_phys(vmm_direct_map_base(), &dm_phys));
	KTEST_ASSERT(dm_phys == 0U);

	/* a canonical but unmapped address must return false */
	uint64_t dummy = 0;
	KTEST_ASSERT(!vmm_virt_to_phys(0x0000010000000000ULL, &dummy));

	/* --- vmm_map / vmm_unmap --- */

	uint64_t page = pmm_alloc_page();
	KTEST_ASSERT(page != PMM_ALLOC_FAILED);

	/* map the page writable and write a magic value through it */
	KTEST_ASSERT(vmm_map(TEST_VIRT, page, VMM_WRITABLE));

	volatile uint64_t *ptr = (volatile uint64_t *)(uintptr_t)TEST_VIRT;
	*ptr = 0xdeadbeefcafeULL;
	KTEST_ASSERT(*ptr == 0xdeadbeefcafeULL);

	/* translation must now report the correct physical address */
	uint64_t mapped_phys = 0;
	KTEST_ASSERT(vmm_virt_to_phys(TEST_VIRT, &mapped_phys));
	KTEST_ASSERT(mapped_phys == page);

	/* unmap and verify translation fails */
	KTEST_ASSERT(vmm_unmap(TEST_VIRT));
	KTEST_ASSERT(!vmm_virt_to_phys(TEST_VIRT, &dummy));

	/* unmapping an already-absent PTE must return false */
	KTEST_ASSERT(!vmm_unmap(TEST_VIRT));

	pmm_free_page(page);

	/* --- vmm_protect --- */

	uint64_t page2 = pmm_alloc_page();
	KTEST_ASSERT(page2 != PMM_ALLOC_FAILED);

	KTEST_ASSERT(vmm_map(TEST_VIRT2, page2, VMM_WRITABLE));

	/* strip write permission; page must still translate */
	KTEST_ASSERT(vmm_protect(TEST_VIRT2, 0));  /* read-only */
	uint64_t prot_phys = 0;
	KTEST_ASSERT(vmm_virt_to_phys(TEST_VIRT2, &prot_phys));
	KTEST_ASSERT(prot_phys == page2);

	/* protect on an unmapped address must return false */
	KTEST_ASSERT(!vmm_protect(0x0000010000000000ULL, 0));

	KTEST_ASSERT(vmm_unmap(TEST_VIRT2));
	pmm_free_page(page2);
}
