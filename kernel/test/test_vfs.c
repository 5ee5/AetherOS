#include "test/ktest.h"
#include "test/tests.h"

#include <stdint.h>

#include "arch/x86_64/apic.h"
#include "core/serial.h"
#include "fs/vfs.h"
#include "sched/sched.h"
#include "mem/pmm.h"
#include "mem/vmm.h"
#include "sched/thread.h"

/* ---- Concurrent VFS access ------------------------------------------------ */

#define VFS_STRESS_THREADS 3
#define VFS_STRESS_ROUNDS  10

/* Generous next to the ~30 small reads the workers actually perform, but bounded
   so a genuinely stuck worker fails the assertion instead of hanging the boot. */
#define VFS_STRESS_TIMEOUT_MS 10000U

/* Per-thread slots rather than shared counters: a plain ++ from several CPUs
   loses updates, and the test would then fail for its own reasons. */
static volatile int s_stress_done[VFS_STRESS_THREADS];
static volatile int s_stress_errs[VFS_STRESS_THREADS];

static void vfs_stress_thread(void *arg)
{
    int idx = (int)(uintptr_t)arg;
    for (int i = 0; i < VFS_STRESS_ROUNDS; ++i) {
        int fd = vfs_open("/hello.txt");
        if (fd < 0) { s_stress_errs[idx]++; break; }
        char buf[32];
        int64_t n = vfs_read(fd, buf, sizeof(buf));
        if (n <= 0) s_stress_errs[idx]++;
        vfs_close(fd);
    }
    s_stress_done[idx] = 1;
}

/* The VFS lock is a mutex, so a contending thread blocks and is woken rather
   than spinning.  That path is untouched by the rest of the suite, which is
   single-threaded -- and a lost wakeup there would hang the whole system rather
   than fail an assertion, so drive it deliberately. */
static void test_vfs_concurrency(void)
{
    for (int i = 0; i < VFS_STRESS_THREADS; ++i) {
        s_stress_done[i] = 0;
        s_stress_errs[i] = 0;
    }
    for (int i = 0; i < VFS_STRESS_THREADS; ++i) {
        thread_create(vfs_stress_thread, (void *)(uintptr_t)i);
    }

    /* Yield rather than spin: the workers only run once this thread gives the
       scheduler a chance, and int $SCHED_YIELD_VECTOR works with IF clear.

       The bound is wall-clock, not a yield count.  A count worked only while
       one CPU ran everything, so each yield handed a worker a turn and 100000
       yields meant 100000 turns of progress.  With the APs actually running
       threads, the workers make progress on another CPU doing real disk I/O
       while this thread burns its whole budget yielding against itself -- so
       the count stopped tracking progress at all and the assertion failed
       intermittently on a slow host.  Time is what we actually mean. */
    int finished = 0;
    uint64_t per_ms   = apic_tsc_per_ms();
    uint64_t deadline = apic_now_tsc() + per_ms * VFS_STRESS_TIMEOUT_MS;
    while (finished < VFS_STRESS_THREADS && apic_now_tsc() < deadline) {
        SCHED_YIELD();
        finished = 0;
        for (int i = 0; i < VFS_STRESS_THREADS; ++i) finished += s_stress_done[i];
    }

    KTEST_ASSERT(finished == VFS_STRESS_THREADS);
    int errs = 0;
    for (int i = 0; i < VFS_STRESS_THREADS; ++i) errs += s_stress_errs[i];
    KTEST_ASSERT(errs == 0);
}

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

    /* vfs_creat creates a file; vfs_unlink removes it. */
    int cr = vfs_creat("/ktest_tmp.txt");
    KTEST_ASSERT(cr == 0);
    KTEST_ASSERT(vfs_file_size("/ktest_tmp.txt") != UINT64_MAX);
    vfs_unlink("/ktest_tmp.txt");
    KTEST_ASSERT(vfs_file_size("/ktest_tmp.txt") == UINT64_MAX);

    /* vfs_rename moves a file; old path disappears, new path appears. */
    vfs_creat("/ktest_rename_src.txt");
    KTEST_ASSERT(vfs_file_size("/ktest_rename_src.txt") != UINT64_MAX);
    int rv = vfs_rename("/ktest_rename_src.txt", "/ktest_rename_dst.txt");
    KTEST_ASSERT(rv == 0);
    KTEST_ASSERT(vfs_file_size("/ktest_rename_src.txt") == UINT64_MAX);
    KTEST_ASSERT(vfs_file_size("/ktest_rename_dst.txt") != UINT64_MAX);

    /* Self-rename must be a no-op — file must survive. */
    rv = vfs_rename("/ktest_rename_dst.txt", "/ktest_rename_dst.txt");
    KTEST_ASSERT(rv == 0);
    KTEST_ASSERT(vfs_file_size("/ktest_rename_dst.txt") != UINT64_MAX);

    vfs_unlink("/ktest_rename_dst.txt");

    /* ---- Relative / normalized path resolution (Phase 1c) ------------------
       The kernel cwd is "/", so each of these must resolve to /hello.txt. */
    KTEST_ASSERT(vfs_file_size("hello.txt")        == sz);  /* relative      */
    KTEST_ASSERT(vfs_file_size("/./hello.txt")     == sz);  /* "." collapse  */
    KTEST_ASSERT(vfs_file_size("/a/../hello.txt")  == sz);  /* ".." collapse */
    KTEST_ASSERT(vfs_file_size("//hello.txt")      == sz);  /* double slash  */
    KTEST_ASSERT(vfs_file_size("/../hello.txt")    == sz);  /* ".." at root  */

    /* ---- Overlong paths must be refused, not overflow a kernel stack buffer --
       path_split() copies the parent prefix into a 256-byte stack buffer, but
       paths arrive here up to 511 bytes and the kernel is built
       -fno-stack-protector.  A ~400-character parent used to smash the stack of
       whichever thread called mkdir/creat/unlink/rename.  Each of these must
       fail cleanly and leave the system standing -- reaching the assertions
       below at all is most of the test. */
    static char longpath[480];
    longpath[0] = '/';
    for (uint32_t i = 1; i < sizeof(longpath) - 3; ++i) longpath[i] = 'a';
    longpath[sizeof(longpath) - 3] = '/';
    longpath[sizeof(longpath) - 2] = 'x';
    longpath[sizeof(longpath) - 1] = '\0';

    KTEST_ASSERT(vfs_mkdir(longpath)  != 0);
    KTEST_ASSERT(vfs_creat(longpath)  != 0);
    KTEST_ASSERT(vfs_unlink(longpath) != 0);
    KTEST_ASSERT(vfs_rename(longpath, "/ktest_nope.txt") != 0);
    KTEST_ASSERT(vfs_rename("/hello.txt", longpath)      != 0);
    /* The real file must be untouched by all of that. */
    KTEST_ASSERT(vfs_file_size("/hello.txt") == sz);

    /* ---- rmdir, and unlink's refusal to eat a directory --------------------
       ext2_unlink had no directory check: it took the link count 2 -> 1, left
       "." and ".." in place, never decremented the parent, and leaked every
       block the directory owned. */
    KTEST_ASSERT(vfs_mkdir("/ktest_dir") == 0);

    /* A directory is not unlinkable. */
    KTEST_ASSERT(vfs_unlink("/ktest_dir") != 0);
    /* ...and it is still there afterwards. */
    KTEST_ASSERT(vfs_mkdir("/ktest_dir") != 0);   /* already exists */

    /* A non-empty directory cannot be rmdir'd. */
    KTEST_ASSERT(vfs_creat("/ktest_dir/f.txt") == 0);
    KTEST_ASSERT(vfs_rmdir("/ktest_dir") != 0);

    /* Empty it, and rmdir succeeds. */
    KTEST_ASSERT(vfs_unlink("/ktest_dir/f.txt") == 0);
    KTEST_ASSERT(vfs_rmdir("/ktest_dir") == 0);
    /* Gone: it can be created again. */
    KTEST_ASSERT(vfs_mkdir("/ktest_dir") == 0);
    KTEST_ASSERT(vfs_rmdir("/ktest_dir") == 0);

    /* rmdir must refuse a regular file and the root directory. */
    KTEST_ASSERT(vfs_rmdir("/hello.txt") != 0);
    KTEST_ASSERT(vfs_rmdir("/") != 0);
    KTEST_ASSERT(vfs_file_size("/hello.txt") == sz);

    test_vfs_concurrency();
}
