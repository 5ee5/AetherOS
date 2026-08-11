#include "test/ktest.h"
#include "test/tests.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "exec/elf.h"
#include "fs/vfs.h"
#include "mem/heap.h"
#include "mem/pmm.h"
#include "mem/vmm.h"
#include "proc/fd.h"
#include "proc/process.h"
#include "sched/thread.h"

/* Must match MAX_PROCS in proc/process.c, which does not export it. */
#define MAX_PROCS_TEST 64

extern const uint8_t hello_elf_start[];
extern const uint8_t hello_elf_end[];

/* Process-lifetime invariants that sys_waitpid depends on: a parent is recorded
   at creation time, and process_reap frees the struct exactly once however many
   callers reach it. */
void test_proc_run(void)
{
    ktest_suite("proc");

    uint64_t size = (uint64_t)(hello_elf_end - hello_elf_start);
    elf_load_result_t r;
    bool loaded = elf_load(hello_elf_start, size, &r);
    KTEST_ASSERT(loaded);
    if (!loaded) return;

    struct process *p = process_create_from_result(&r);
    KTEST_ASSERT(p != NULL);
    if (!p) { vmm_space_destroy(r.cr3); return; }

    uint32_t pid = p->pid;
    struct thread *t = p->thread;

    KTEST_ASSERT(pid != 0);
    /* Created from a kernel thread, which owns no process, so there is no
       parent to record.  sys_waitpid rejects every caller whose pid does not
       match this field, so a stale or wrong ppid would let one process wait on
       — and via the Ctrl+C path kill — an unrelated one. */
    KTEST_ASSERT(p->ppid == 0);
    KTEST_ASSERT(process_find(pid) == p);
    /* process_start() was not called, so the thread is parked off the runqueue
       and the scheduler's deferred reaper will never see it. */
    KTEST_ASSERT(t != NULL);
    KTEST_ASSERT(t->is_user);

    /* First reap releases the table slot and frees the struct. */
    process_reap(p);
    KTEST_ASSERT(process_find(pid) == NULL);

    /* A second reap of the same (now dangling) pointer must be a no-op.  Before
       the table slot was claimed under proc_table_lock, both callers reached
       kfree and the second free tripped the heap magic check and panicked.
       process_reap compares the pointer without dereferencing it, so passing
       the freed pointer here is exactly the race being guarded. */
    process_reap(p);
    KTEST_ASSERT(process_find(pid) == NULL);

    /* Release what the parked thread owns, since nothing else will. */
    vmm_space_destroy(r.cr3);
    kfree(t->kstack);
    kfree(t);

    /* Exiting must return file descriptors to the VFS open-file table, which is
       a global 16-entry array.  Loop well past that: if process teardown leaks
       FD_FILE slots (it used to close only pipe ends), vfs_open starts failing
       once the table fills and never recovers. */
    if (vfs_file_size("/hello.txt") != UINT64_MAX) {
        bool all_opened = true;
        for (int i = 0; i < 64; i++) {
            struct process holder;
            fd_table_init(&holder.fds);
            int vfd = vfs_open("/hello.txt");
            if (vfd < 0) { all_opened = false; break; }
            if (fd_alloc(&holder.fds, vfd) < 0) { all_opened = false; break; }
            process_close_all_fds(&holder);   /* what exit and kill both call */
        }
        KTEST_ASSERT(all_opened);
    }

    /* Every allocator must reserve 0/1/2 for stdio.  fd_alloc_socket used to
       start at 0, so the first socket in a fresh process landed on stdin and
       read() served it keyboard input instead of the network (wget read the
       keyboard).  Assert the floor for all three kinds. */
    {
        struct process holder;
        fd_table_init(&holder.fds);
        int sfd = fd_alloc_socket(&holder.fds, 0);
        int ffd = fd_alloc(&holder.fds, 0);
        int pfd = fd_alloc_pipe(&holder.fds, 0, FD_PIPE_READ);
        KTEST_ASSERT(sfd >= 3 && ffd >= 3 && pfd >= 3);
        /* Distinct slots, and stdio still untouched. */
        KTEST_ASSERT(sfd != ffd && ffd != pfd && sfd != pfd);
        KTEST_ASSERT(fd_type(&holder.fds, 0) == FD_NONE);
        KTEST_ASSERT(fd_type(&holder.fds, 1) == FD_NONE);
        KTEST_ASSERT(fd_type(&holder.fds, 2) == FD_NONE);
        KTEST_ASSERT(fd_type(&holder.fds, sfd) == FD_SOCKET);
    }

    /* A process must never exist unpublished.  With the table full,
       process_create_from_result used to skip the insert but return the process
       anyway, so it ran while invisible to ps, unwaitable and never reaped.  Fill
       every slot, then check the next creation is refused outright — and that it
       does not leak the address space it was handed. */
    {
        struct process *filler[MAX_PROCS_TEST];
        int made = 0;
        for (; made < MAX_PROCS_TEST; ++made) {
            elf_load_result_t fr;
            if (!elf_load(hello_elf_start, size, &fr)) break;
            struct process *fp = process_create_from_result(&fr);
            if (fp == NULL) { vmm_space_destroy(fr.cr3); break; }
            filler[made] = fp;
        }
        /* The table is full now (or we ran out of heap first, which is also a
           refusal rather than a silent success — either way nothing started
           unpublished). */
        /* Refuse repeatedly.  The first attempt may grow the kernel heap (which
           never returns pages to the PMM), so measure from the second: once the
           high-water mark is reached, any further drop in free pages means a
           refused attempt leaked the address space it was handed. */
        uint64_t after_first = 0;
        for (int attempt = 0; attempt < 4; ++attempt) {
            elf_load_result_t xr;
            if (!elf_load(hello_elf_start, size, &xr)) break;
            struct process *extra = process_create_from_result(&xr);
            KTEST_ASSERT(extra == NULL);
            if (extra != NULL) {
                /* Would-be regression: clean up so later suites still run. */
                process_mark_exited(extra, 0);
                process_reap(extra);
                break;
            }
            if (attempt == 0) {
                after_first = pmm_free_pages();
            } else {
                KTEST_ASSERT(pmm_free_pages() == after_first);
            }
        }
        for (int i = 0; i < made; ++i) {
            struct thread *ft = filler[i]->thread;
            vmm_space_destroy(filler[i]->cr3);
            process_mark_exited(filler[i], 0);
            process_reap(filler[i]);
            kfree(ft->kstack);
            kfree(ft);
        }
    }
}
