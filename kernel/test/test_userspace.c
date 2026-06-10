#include "test/ktest.h"
#include "test/tests.h"

#include <stdbool.h>
#include <stdint.h>

#include "exec/elf.h"
#include "mem/pmm.h"
#include "mem/uaccess.h"
#include "mem/vmm.h"

extern const uint8_t hello_elf_start[];
extern const uint8_t hello_elf_end[];

void test_userspace_run(void)
{
    ktest_suite("userspace");

    /* The embedded ELF must be non-empty. */
    uint64_t size = (uint64_t)(hello_elf_end - hello_elf_start);
    KTEST_ASSERT(size > 0);

    /* elf_load must succeed on the embedded hello binary. */
    uint64_t pages_before = pmm_free_pages();
    elf_load_result_t r;
    bool ok = elf_load(hello_elf_start, size, &r);
    KTEST_ASSERT(ok);
    if (ok) {
        /* Entry point must be in user address space. */
        KTEST_ASSERT(r.entry < 0x800000000000ULL);
        /* New address space must differ from kernel PML4. */
        KTEST_ASSERT(r.cr3 != vmm_pml4_phys());
        KTEST_ASSERT(r.cr3 != 0);
        /* elf_load must have consumed pages. */
        KTEST_ASSERT(pmm_free_pages() < pages_before);
        /* vmm_space_destroy must recover all allocated pages. */
        vmm_space_destroy(r.cr3);
        KTEST_ASSERT(pmm_free_pages() == pages_before);
    }

    /* elf_load must reject a bad ELF magic. */
    static const uint8_t bad[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    elf_load_result_t r2;
    bool ok2 = elf_load(bad, sizeof(bad), &r2);
    KTEST_ASSERT(!ok2);

    /* ---- copy_to/from_user fault recovery (Phase 3) -------------------- */

    /* Above the user limit: rejected by the range check. */
    char kbuf[8];
    KTEST_ASSERT(copy_from_user(kbuf, (const void *)0x900000000000ULL, 4) == false);
    KTEST_ASSERT(copy_to_user((void *)0x900000000000ULL, kbuf, 4) == false);

    /* Map a real user-range page in the current address space and verify both
       directions succeed, then unmap and verify the access now faults and is
       recovered (returns false) instead of panicking the kernel. */
    uint64_t uphys = pmm_alloc_page();
    KTEST_ASSERT(uphys != PMM_ALLOC_FAILED);
    if (uphys != PMM_ALLOC_FAILED) {
        const uint64_t uva = 0x555555000000ULL; /* high user range, unmapped */
        KTEST_ASSERT(vmm_map(uva, uphys, VMM_WRITABLE | VMM_USER));
        uint8_t *dm = (uint8_t *)(uintptr_t)(vmm_direct_map_base() + uphys);

        /* copy_to_user writes the user page. */
        KTEST_ASSERT(copy_to_user((void *)uva, "Hi", 3) == true);
        KTEST_ASSERT(dm[0] == 'H' && dm[1] == 'i' && dm[2] == '\0');

        /* copy_from_user reads it back. */
        char got[4] = {0};
        KTEST_ASSERT(copy_from_user(got, (const void *)uva, 3) == true);
        KTEST_ASSERT(got[0] == 'H' && got[1] == 'i' && got[2] == '\0');

        /* strncpy_from_user honors the NUL terminator. */
        char s[8] = {0};
        KTEST_ASSERT(strncpy_from_user(s, (const void *)uva, sizeof(s)) == 2);

        /* After unmapping, the same access must fault and recover. */
        KTEST_ASSERT(vmm_unmap(uva));
        KTEST_ASSERT(copy_from_user(got, (const void *)uva, 3) == false);
        KTEST_ASSERT(copy_to_user((void *)uva, "zz", 2) == false);

        pmm_free_page(uphys);
    }
}
