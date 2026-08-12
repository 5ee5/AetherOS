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

	/* --- NX reaches the hardware PTE --- */

	/* VMM_NX is bit 63, well outside the flag range the rest of the mapping
	   path handles, so it is exactly the kind of flag a mask could silently
	   drop — leaving NX enabled in EFER but protecting nothing.  Read the PTE
	   back rather than trusting the call. */
	uint64_t page3 = pmm_alloc_page();
	KTEST_ASSERT(page3 != PMM_ALLOC_FAILED);
	if (page3 != PMM_ALLOC_FAILED) {
		KTEST_ASSERT(vmm_map(TEST_VIRT2, page3, VMM_WRITABLE | VMM_NX));

		uint64_t pte = 0;
		KTEST_ASSERT(vmm_pte_peek(TEST_VIRT2, &pte));
		KTEST_ASSERT((pte & VMM_NX) != 0);
		/* ...and the mapping is otherwise intact. */
		uint64_t nx_phys = 0;
		KTEST_ASSERT(vmm_virt_to_phys(TEST_VIRT2, &nx_phys));
		KTEST_ASSERT(nx_phys == page3);

		/* A mapping without VMM_NX must stay executable. */
		KTEST_ASSERT(vmm_map(TEST_VIRT2, page3, VMM_WRITABLE));
		KTEST_ASSERT(vmm_pte_peek(TEST_VIRT2, &pte));
		KTEST_ASSERT((pte & VMM_NX) == 0);

		KTEST_ASSERT(vmm_unmap(TEST_VIRT2));
		pmm_free_page(page3);
	}

	/* The direct map must be non-executable: it aliases all of physical
	   memory, so an executable alias undermines SMEP and NX together. */
	uint64_t dm_pte = 0;
	KTEST_ASSERT(vmm_pde_peek(vmm_direct_map_base() + 0x100000ULL, &dm_pte));
	KTEST_ASSERT((dm_pte & VMM_NX) != 0);

	/* --- vmm_current_cr3 reports the installed space, vmm_pml4_phys does not ---

	   These two were confused in sys_spawn: it saved vmm_pml4_phys() as the
	   address space to go back to, which is the kernel's boot-time PML4 rather
	   than the caller's, so a user thread was put on the kernel page tables and
	   faulted at its next instruction fetch in ring 3 (audit H23).  Assert the
	   distinction so a future caller cannot re-use the wrong one unnoticed. */
	KTEST_ASSERT(vmm_current_cr3() == vmm_pml4_phys());   /* on a kernel thread */

	uint64_t space = vmm_space_create();
	KTEST_ASSERT(space != 0);
	if (space != 0) {
		KTEST_ASSERT(space != vmm_pml4_phys());
		/* No preemption inside the window: sched_tick reinstalls the outgoing
		   thread's own CR3, which would repair the switch before we look. */
		__asm__ volatile("cli" ::: "memory");
		vmm_space_switch(space);
		uint64_t seen_live  = vmm_current_cr3();
		uint64_t seen_stale = vmm_pml4_phys();
		vmm_space_switch(seen_stale);
		__asm__ volatile("sti" ::: "memory");

		KTEST_ASSERT(seen_live == space);          /* tracks the real CR3 */
		KTEST_ASSERT(seen_stale != space);         /* the snapshot does not */
		KTEST_ASSERT(vmm_current_cr3() == vmm_pml4_phys());  /* restored */

		vmm_space_destroy(space);
	}
}
