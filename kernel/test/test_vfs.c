#include "test/ktest.h"
#include "test/tests.h"

#include <stdint.h>

#include "core/serial.h"
#include "fs/vfs.h"
#include "mem/pmm.h"
#include "mem/vmm.h"

void test_vfs_run(void)
{
    ktest_suite("vfs");

    /* vfs_init must have been called before the test suite runs.
       If no disk is present the suite is skipped gracefully. */
    uint64_t sz = vfs_file_size("/hello.txt");
    if (sz == UINT64_MAX) {
        /* No filesystem mounted or file absent — skip. */
        serial_write("vfs: /hello.txt not found, skipping tests\n");
        return;
    }

    KTEST_ASSERT(sz > 0);

    /* Open the file. */
    int fd = vfs_open("/hello.txt");
    KTEST_ASSERT(fd >= 0);

    if (fd >= 0) {
        /* Allocate a PMM page for the read buffer (must be in the direct map). */
        uint64_t buf_phys = pmm_alloc_page();
        KTEST_ASSERT(buf_phys != PMM_ALLOC_FAILED);

        if (buf_phys != PMM_ALLOC_FAILED) {
            uint8_t *buf = (uint8_t *)(uintptr_t)(vmm_direct_map_base() + buf_phys);
            int64_t n = vfs_read(fd, buf, 4096);
            KTEST_ASSERT(n > 0);
            KTEST_ASSERT((uint64_t)n == sz);

            /* The file must contain the expected content. */
            const char *expect = "Hello from ext2!";
            uint32_t elen = 16;
            KTEST_ASSERT((uint64_t)n >= elen);
            bool match = true;
            for (uint32_t i = 0; i < elen; ++i) {
                if (buf[i] != (uint8_t)expect[i]) { match = false; break; }
            }
            KTEST_ASSERT(match);

            pmm_free_page(buf_phys);
        }

        vfs_close(fd);
    }

    /* Open a non-existent file must return negative. */
    int bad = vfs_open("/no_such_file.txt");
    KTEST_ASSERT(bad < 0);
}
