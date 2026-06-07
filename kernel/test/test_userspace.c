#include "test/ktest.h"
#include "test/tests.h"

#include <stdbool.h>
#include <stdint.h>

#include "exec/elf.h"
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
    elf_load_result_t r;
    bool ok = elf_load(hello_elf_start, size, &r);
    KTEST_ASSERT(ok);
    if (ok) {
        /* Entry point must be in user address space. */
        KTEST_ASSERT(r.entry < 0x800000000000ULL);
        /* New address space must differ from kernel PML4. */
        KTEST_ASSERT(r.cr3 != vmm_pml4_phys());
        KTEST_ASSERT(r.cr3 != 0);
        /* Clean up. */
        vmm_space_destroy(r.cr3);
    }

    /* elf_load must reject a bad ELF magic. */
    static const uint8_t bad[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    elf_load_result_t r2;
    bool ok2 = elf_load(bad, sizeof(bad), &r2);
    KTEST_ASSERT(!ok2);
}
