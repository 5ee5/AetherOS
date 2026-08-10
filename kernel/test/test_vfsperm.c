#include "test/ktest.h"
#include "test/tests.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/serial.h"
#include "fs/vfs.h"
#include "lib/string.h"
#include "proc/fd.h"
#include "proc/process.h"
#include "sched/sched.h"
#include "sched/thread.h"
#include "security/cred.h"

#define O_CREAT_FLAG 0x40

/* The VFS grants kernel context (no current process) unrestricted access, so
   exercising the permission checks means borrowing the running kernel thread
   and giving it credentials.  Restored before returning. */
static void set_fake_creds(struct process *fake, uint32_t uid, uint32_t gid)
{
    cred_init(&fake->cred, uid, gid);
    fd_table_init(&fake->fds);
    fake->pid = 0;
    fake->ppid = 0;
    fake->cwd[0] = '/';
    fake->cwd[1] = '\0';
    fake->name[0] = '\0';
    sched_current()->process = fake;
}

/* Guards the fix for the privilege escalation where an unprivileged user could
   unlink /etc/passwd — which sys_open's mode check does not cover, since that
   check only fires for files that already exist — and recreate it with their
   own contents to become root. */
void test_vfsperm_run(void)
{
    ktest_suite("vfsperm");

    if (vfs_file_size("/etc/passwd") == UINT64_MAX) {
        serial_write("vfsperm: /etc/passwd absent, skipping tests\n");
        return;
    }

    struct thread *self = sched_current();
    void *saved = self->process;
    struct process fake;

    /* /etc and / are 0755 root-owned, so uid 1000 has r-x and no write. */
    set_fake_creds(&fake, 1000, 1000);

    KTEST_ASSERT(vfs_unlink("/etc/passwd") != 0);
    KTEST_ASSERT(vfs_creat("/etc/ktest_evil") != 0);
    KTEST_ASSERT(vfs_mkdir("/etc/ktest_evil") != 0);
    KTEST_ASSERT(vfs_rename("/etc/passwd", "/etc/ktest_evil") != 0);
    KTEST_ASSERT(vfs_open_ex("/etc/ktest_evil", O_CREAT_FLAG) < 0);
    /* Displacing a setuid binary is the same escalation by another route. */
    KTEST_ASSERT(vfs_rename("/bin/sudo", "/bin/ktest_evil") != 0);

    /* The denials above must not have mutated anything. */
    self->process = saved;
    KTEST_ASSERT(vfs_file_size("/etc/passwd") != UINT64_MAX);
    KTEST_ASSERT(vfs_file_size("/etc/ktest_evil") == UINT64_MAX);

    /* Root is still allowed: create then remove, leaving the tree unchanged. */
    set_fake_creds(&fake, 0, 0);
    KTEST_ASSERT(vfs_creat("/etc/ktest_root") == 0);
    KTEST_ASSERT(vfs_file_size("/etc/ktest_root") != UINT64_MAX);
    KTEST_ASSERT(vfs_unlink("/etc/ktest_root") == 0);

    self->process = saved;
    KTEST_ASSERT(vfs_file_size("/etc/ktest_root") == UINT64_MAX);
}
