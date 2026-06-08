#include "syscall/syscall.h"

#include <stddef.h>
#include <stdint.h>

#include "core/serial.h"
#include "exec/elf.h"
#include "fs/vfs.h"
#include "lib/string.h"
#include "mem/heap.h"
#include "mem/vmm.h"
#include "proc/fd.h"
#include "proc/process.h"
#include "sched/sched.h"
#include "sched/thread.h"

#define IA32_EFER  0xc0000080U
#define IA32_STAR  0xc0000081U
#define IA32_LSTAR 0xc0000082U
#define IA32_FMASK 0xc0000084U

/* syscall entry point declared in syscall_entry.asm */
extern void syscall_entry(void);

static void wrmsr(uint32_t msr, uint64_t value)
{
    uint32_t lo = (uint32_t)(value & 0xffffffffULL);
    uint32_t hi = (uint32_t)(value >> 32U);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32U) | lo;
}

void syscall_init(void)
{
    /* Enable SYSCALL/SYSRET: set SCE bit in IA32_EFER. */
    wrmsr(IA32_EFER, rdmsr(IA32_EFER) | 1U);

    /* STAR[47:32] = kernel CS (0x08); SYSRET uses base 0x20:
       SYSRET SS = (0x20+8)|3 = 0x2b, CS64 = (0x20+16)|3 = 0x33. */
    wrmsr(IA32_STAR, (0x0020ULL << 48U) | (0x0008ULL << 32U));

    /* LSTAR = syscall entry RIP. */
    wrmsr(IA32_LSTAR, (uint64_t)(uintptr_t)syscall_entry);

    /* FMASK: mask IF on SYSCALL entry (cleared; re-enabled on SYSRETQ). */
    wrmsr(IA32_FMASK, 0x200ULL);

    serial_write("syscall: SYSCALL/SYSRET enabled\n");
}

/* ---- Syscall handlers -------------------------------------------------- */

#define SYS_READ      0U
#define SYS_WRITE     1U
#define SYS_OPEN      2U
#define SYS_CLOSE     3U
#define SYS_GETPID   39U
#define SYS_EXIT     60U
#define SYS_WAITPID  61U
#define SYS_GETUID  102U
#define SYS_GETGID  104U
#define SYS_GETEUID 107U
#define SYS_GETEGID 108U
#define SYS_SPAWN   500U
#define SYS_LISTDIR 600U

static int64_t sys_write(uint64_t fd, uint64_t buf_virt, uint64_t count)
{
    /* Only fd=1 (stdout) supported; write to serial. */
    if (fd != 1U) {
        return -9;   /* -EBADF */
    }
    /* Basic bounds check: user pointers must be below the kernel half. */
    if (buf_virt + count < buf_virt || buf_virt + count > 0x800000000000ULL) {
        return -14;  /* -EFAULT */
    }
    const char *p = (const char *)(uintptr_t)buf_virt;
    for (uint64_t i = 0; i < count; ++i) {
        serial_write_char(p[i]);
    }
    return (int64_t)count;
}

static fd_table_t *current_fds(void)
{
    struct thread *t = sched_current();
    if (!t || !t->process) return NULL;
    struct process *p = (struct process *)t->process;
    return &p->fds;
}

static int64_t sys_open(uint64_t path_virt, uint64_t flags, uint64_t mode)
{
    (void)flags; (void)mode;
    if (path_virt >= 0x800000000000ULL) return -14; /* EFAULT */
    const char *path = (const char *)(uintptr_t)path_virt;
    int vfd = vfs_open(path);
    if (vfd < 0) return -2; /* ENOENT */
    fd_table_t *fds = current_fds();
    if (!fds) { vfs_close(vfd); return -9; }
    int fd = fd_alloc(fds, vfd);
    if (fd < 0) { vfs_close(vfd); return -24; } /* EMFILE */
    return fd;
}

static int64_t sys_read_stdin(void *buf, uint32_t count)
{
    uint8_t *p = (uint8_t *)buf;
    uint32_t n = 0;
    while (n < count) {
        __asm__ volatile("sti" ::: "memory");   /* allow timer IRQ so int $0x20 yields */
        char c = serial_read_char();
        if (!c) {
            __asm__ volatile("int $0x20" ::: "memory");  /* yield, stay READY */
            continue;
        }
        if (c == '\r') c = '\n';
        if (c == 4) {            /* Ctrl+D = EOF */
            if (n > 0) break;
            return 0;
        }
        p[n++] = (uint8_t)c;
        if (c == '\n') break;
    }
    return (int64_t)n;
}

static int64_t sys_read(uint64_t fd, uint64_t buf_virt, uint64_t count)
{
    if (buf_virt + count < buf_virt || buf_virt + count > 0x800000000000ULL) return -14;
    void *buf = (void *)(uintptr_t)buf_virt;
    if (fd == 0) return sys_read_stdin(buf, (uint32_t)count);
    fd_table_t *fds = current_fds();
    if (!fds) return -9;
    int vfd = fd_to_vfs(fds, (int)fd);
    if (vfd < 0) return -9;
    return vfs_read(vfd, buf, (uint32_t)count);
}

static int64_t sys_close(uint64_t fd)
{
    fd_table_t *fds = current_fds();
    if (!fds) return -9;
    int vfd = fd_to_vfs(fds, (int)fd);
    if (vfd < 0) return -9;
    vfs_close(vfd);
    fd_free(fds, (int)fd);
    return 0;
}

static int64_t sys_exit(uint64_t status)
{
    struct thread *t = sched_current();
    struct process *proc = (struct process *)t->process;

    /* Record exit status and wake any sys_waitpid callers. */
    if (proc) {
        proc->exit_status = (int32_t)(status & 0xffU);
        proc->exited = true;
        struct thread *w = proc->wait_queue;
        proc->wait_queue = NULL;
        while (w) {
            struct thread *nxt = w->wait_next;
            w->wait_next = NULL;
            sched_wake(w);
            w = nxt;
        }
    }

    if (t->is_user && t->cr3 != 0) {
        vmm_space_destroy(t->cr3);
        t->cr3 = 0;
    }

    /* Disable interrupts, mark dead, and force a reschedule. */
    __asm__ volatile("cli" ::: "memory");
    t->state = THREAD_DEAD;
    __asm__ volatile("int $0x20" ::: "memory");
    /* Never reached. */
    for (;;) {
        __asm__ volatile("hlt");
    }
    return 0;
}

static int64_t sys_waitpid(uint64_t pid, uint64_t status_virt, uint64_t options)
{
    (void)options;
    struct process *target = process_find((uint32_t)pid);
    if (!target) return -10;   /* ECHILD */
    struct thread *t = sched_current();
    while (!target->exited) {
        t->wait_next = target->wait_queue;
        target->wait_queue = t;
        t->state = THREAD_BLOCKED;
        __asm__ volatile("cli; int $0x20; sti" ::: "memory");
    }
    if (status_virt && status_virt < 0x800000000000ULL)
        *(int32_t *)(uintptr_t)status_virt = target->exit_status << 8;
    return (int64_t)pid;
}

#define MAX_SPAWN_ARGS 16
#define MAX_ARG_LEN    256

static int64_t sys_spawn(uint64_t path_virt, uint64_t argv_virt)
{
    if (path_virt >= 0x800000000000ULL) return -14;
    const char *path = (const char *)(uintptr_t)path_virt;

    uint64_t fsize = vfs_file_size(path);
    if (fsize == UINT64_MAX || fsize == 0 || fsize > 4U * 1024U * 1024U) return -2;
    void *buf = kmalloc((uint32_t)fsize);
    if (!buf) return -12;
    int vfd = vfs_open(path);
    if (vfd < 0) { kfree(buf); return -2; }
    vfs_read(vfd, buf, (uint32_t)fsize);
    vfs_close(vfd);

    elf_load_result_t result;
    if (!elf_load(buf, fsize, &result)) { kfree(buf); return -1; }
    kfree(buf);

    /* Copy argv strings from user space into kernel buffers. */
    static char arg_store[MAX_SPAWN_ARGS][MAX_ARG_LEN];
    int argc = 0;
    if (argv_virt && argv_virt < 0x800000000000ULL) {
        uint64_t *argv = (uint64_t *)(uintptr_t)argv_virt;
        while (argc < MAX_SPAWN_ARGS) {
            uint64_t p = argv[argc];
            if (!p || p >= 0x800000000000ULL) break;
            const char *src = (const char *)(uintptr_t)p;
            uint32_t len = 0;
            while (len < MAX_ARG_LEN - 1U && src[len]) len++;
            memcpy(arg_store[argc], src, len);
            arg_store[argc][len] = '\0';
            argc++;
        }
    }

    /* Write ABI initial stack into child's address space.
       Switch to the child's CR3 so user virtual addresses are accessible.
       Kernel half is identical (copied at vmm_space_create time). */
    uint64_t old_cr3 = vmm_pml4_phys();
    vmm_space_switch(result.cr3);

    uint64_t sp = result.user_sp;  /* 0x7ffffffffffff0 */

    /* Pack strings from top downward. */
    uint64_t str_ptrs[MAX_SPAWN_ARGS];
    for (int i = argc - 1; i >= 0; i--) {
        uint32_t len = (uint32_t)strlen(arg_store[i]) + 1U;
        sp -= len;
        sp &= ~7ULL;
        memcpy((void *)(uintptr_t)sp, arg_store[i], len);
        str_ptrs[i] = sp;
    }

    sp &= ~0xfULL;     /* 16-byte align before pointer array */

    /* NULL sentinel at end of argv. */
    sp -= 8;
    *(uint64_t *)(uintptr_t)sp = 0;

    /* argv pointers (argv[argc-1] … argv[0], so [0] is at the top). */
    for (int i = argc - 1; i >= 0; i--) {
        sp -= 8;
        *(uint64_t *)(uintptr_t)sp = str_ptrs[i];
    }

    /* argc value — crt0 does `pop rdi` to read it. */
    sp -= 8;
    *(uint64_t *)(uintptr_t)sp = (uint64_t)argc;

    vmm_space_switch(old_cr3);

    result.user_sp = sp;
    struct process *proc = process_create_from_result(&result);
    return proc ? (int64_t)proc->pid : -1;
}

static int64_t sys_listdir(uint64_t path_virt, uint64_t buf_virt, uint64_t bufsz)
{
    if (path_virt >= 0x800000000000ULL || buf_virt >= 0x800000000000ULL) return -14;
    return (int64_t)vfs_listdir((const char *)(uintptr_t)path_virt,
                                 (char *)(uintptr_t)buf_virt, (uint32_t)bufsz);
}

int64_t syscall_dispatch(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4)
{
    (void)a3; (void)a4;
    switch (nr) {
    case SYS_READ:    return sys_read(a0, a1, a2);
    case SYS_WRITE:   return sys_write(a0, a1, a2);
    case SYS_OPEN:    return sys_open(a0, a1, a2);
    case SYS_CLOSE:   return sys_close(a0);
    case SYS_GETPID:  return (int64_t)sched_current()->tid;
    case SYS_EXIT:    return sys_exit(a0);
    case SYS_WAITPID: return sys_waitpid(a0, a1, a2);
    case SYS_GETUID:  { struct process *p = (struct process *)sched_current()->process; return p ? (int64_t)p->cred.uid  : 0; }
    case SYS_GETGID:  { struct process *p = (struct process *)sched_current()->process; return p ? (int64_t)p->cred.gid  : 0; }
    case SYS_GETEUID: { struct process *p = (struct process *)sched_current()->process; return p ? (int64_t)p->cred.euid : 0; }
    case SYS_GETEGID: { struct process *p = (struct process *)sched_current()->process; return p ? (int64_t)p->cred.egid : 0; }
    case SYS_SPAWN:   return sys_spawn(a0, a1);
    case SYS_LISTDIR: return sys_listdir(a0, a1, a2);
    default:
        serial_write("syscall: unknown nr=");
        serial_write_dec(nr);
        serial_write("\n");
        return -38;   /* -ENOSYS */
    }
}
