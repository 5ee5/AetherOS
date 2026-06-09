#include "syscall/syscall.h"

#include <stddef.h>
#include <stdint.h>

#include "acpi/acpi.h"
#include "core/serial.h"
#include "exec/elf.h"
#include "fs/vfs.h"
#include "lib/string.h"
#include "mem/heap.h"
#include "mem/vmm.h"
#include "net/dns.h"
#include "net/socket.h"
#include "proc/fd.h"
#include "proc/pipe.h"
#include "proc/process.h"
#include "sched/sched.h"
#include "sched/thread.h"
#include "os/socket.h"
#include "security/cred.h"

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
#define SYS_STAT      4U
#define SYS_GETCWD   79U
#define SYS_CHDIR    80U
#define SYS_MKDIR    83U
#define SYS_UNLINK   87U
#define SYS_SOCKET   41U
#define SYS_CONNECT  42U
#define SYS_SEND     44U
#define SYS_RECV     45U
#define SYS_PIPE      22U
#define SYS_CHOWN     92U
#define SYS_SETUID   105U
#define SYS_SETGID   106U
#define SYS_SLEEP     35U
#define SYS_KILL      62U
#define SYS_CHMOD     90U
#define SYS_RENAME    82U
#define SYS_SPAWN    500U
#define SYS_SPAWN_AS 501U
#define SYS_LISTDIR  600U
#define SYS_CREAT    601U
#define SYS_DNS      602U
#define SYS_PS       603U

static fd_table_t *current_fds(void);

static int64_t sys_write(uint64_t fd, uint64_t buf_virt, uint64_t count)
{
    if (buf_virt + count < buf_virt || buf_virt + count > 0x800000000000ULL)
        return -14; /* -EFAULT */
    const char *p = (const char *)(uintptr_t)buf_virt;

    if (fd == 1U || fd == 2U) {
        /* Check if stdout is redirected to a pipe in this process. */
        if (fd == 1U) {
            fd_table_t *fds = current_fds();
            if (fds && fd_type(fds, 1) == FD_PIPE_WRITE) {
                int pidx = fd_id(fds, 1);
                uint32_t written = 0;
                while (written < count) {
                    int n = pipe_write(pidx, p + written, (uint32_t)(count - written));
                    if (n < 0) return (int64_t)written;
                    written += (uint32_t)n;
                    if (written < count)
                        __asm__ volatile("int $0x20" ::: "memory"); /* yield if buffer full */
                }
                return (int64_t)count;
            }
        }
        for (uint64_t i = 0; i < count; ++i) serial_write_char(p[i]);
        return (int64_t)count;
    }

    fd_table_t *fds = current_fds();
    if (!fds) return -9;

    fd_type_t type = fd_type(fds, (int)fd);
    if (type == FD_SOCKET) {
        int idx = fd_id(fds, (int)fd);
        return (int64_t)sock_send(idx, p, (uint32_t)count);
    }

    int vfd = fd_to_vfs(fds, (int)fd);
    if (vfd < 0) return -9;
    return vfs_write(vfd, p, (uint32_t)count);
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
    (void)mode;
    if (path_virt >= 0x800000000000ULL) return -14; /* EFAULT */
    const char *path = (const char *)(uintptr_t)path_virt;

    /* Check read/write permission on existing files. */
    {
        struct process *cp = sched_current_process();
        if (cp) {
            uint16_t fmode = 0; uint32_t fuid = 0, fgid = 0;
            if (vfs_file_stat(path, &fmode, &fuid, &fgid) == 0) {
                uint8_t access;
                uint32_t rw = flags & 3u;
                if      (rw == 1u) access = 2u;  /* O_WRONLY → write */
                else if (rw == 2u) access = 6u;  /* O_RDWR  → read+write */
                else               access = 4u;  /* O_RDONLY → read */
                if (!cred_check(&cp->cred, fuid, fgid, fmode, access))
                    return -13; /* EACCES */
            }
            /* If stat fails the file doesn't exist; O_CREAT will handle it. */
        }
    }

    int vfd = vfs_open_ex(path, (int)flags);
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
    if (fd == 0) {
        fd_table_t *fds = current_fds();
        if (fds && fd_type(fds, 0) == FD_PIPE_READ) {
            int pidx = fd_id(fds, 0);
            int n;
            while ((n = pipe_read(pidx, buf, (uint32_t)count)) == -2)
                __asm__ volatile("int $0x20" ::: "memory");
            return (n >= 0) ? (int64_t)n : -1;
        }
        return sys_read_stdin(buf, (uint32_t)count);
    }

    fd_table_t *fds = current_fds();
    if (!fds) return -9;

    fd_type_t type = fd_type(fds, (int)fd);
    if (type == FD_SOCKET) {
        int idx = fd_id(fds, (int)fd);
        int n;
        while ((n = sock_recv(idx, buf, (uint32_t)count)) == 0)
            __asm__ volatile("int $0x20" ::: "memory");
        if (n < 0) return 0; /* EOF */
        return (int64_t)n;
    }

    int vfd = fd_to_vfs(fds, (int)fd);
    if (vfd < 0) return -9;
    return vfs_read(vfd, buf, (uint32_t)count);
}

static int64_t sys_close(uint64_t fd)
{
    fd_table_t *fds = current_fds();
    if (!fds) return -9;

    fd_type_t type = fd_type(fds, (int)fd);
    if (type == FD_SOCKET) {
        sock_close(fd_id(fds, (int)fd));
        fd_free(fds, (int)fd);
        return 0;
    }
    if (type == FD_PIPE_READ) {
        pipe_close_read(fd_id(fds, (int)fd));
        fd_free(fds, (int)fd);
        return 0;
    }
    if (type == FD_PIPE_WRITE) {
        pipe_close_write(fd_id(fds, (int)fd));
        fd_free(fds, (int)fd);
        return 0;
    }

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
        /* Close all pipe fds so connected readers/writers see EOF. */
        for (int i = 0; i < MAX_FDS; i++) {
            if (!proc->fds.fds[i].open) continue;
            fd_type_t ft = proc->fds.fds[i].type;
            if (ft == FD_PIPE_READ)  pipe_close_read(proc->fds.fds[i].id);
            if (ft == FD_PIPE_WRITE) pipe_close_write(proc->fds.fds[i].id);
            proc->fds.fds[i].open = false;
        }
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
        /* Non-blocking serial peek: if user sends Ctrl+C, kill the process. */
        char c = serial_read_char();
        if (c == 0x03) {
            process_kill(target);
            serial_write("^C\n");
            break;
        }
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

static int64_t sys_spawn(uint64_t path_virt, uint64_t argv_virt,
                         int64_t stdin_fd_parent, int64_t stdout_fd_parent)
{
    if (path_virt >= 0x800000000000ULL) return -14;
    const char *path = (const char *)(uintptr_t)path_virt;

    /* Check execute permission before loading. */
    {
        struct process *cp = sched_current_process();
        if (cp) {
            uint16_t fmode = 0; uint32_t fuid = 0, fgid = 0;
            if (vfs_file_stat(path, &fmode, &fuid, &fgid) == 0 &&
                !cred_check(&cp->cred, fuid, fgid, fmode, 1u))
                return -13; /* EACCES */
        }
    }

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
    if (!proc) return -1;

    /* Store process name from the basename of path. */
    {
        const char *base = path;
        for (const char *p = path; *p; p++)
            if (*p == '/') base = p + 1;
        uint32_t i = 0;
        while (i < 31 && base[i]) { proc->name[i] = base[i]; i++; }
        proc->name[i] = '\0';
    }

    /* Inherit credentials from parent. */
    {
        struct thread *ct = sched_current();
        struct process *parent = ct ? (struct process *)ct->process : NULL;
        if (parent) proc->cred = parent->cred;
    }

    /* Setuid: if the executable's inode has the setuid bit (04000), elevate euid. */
    {
        uint16_t fmode = 0; uint32_t fuid = 0, fgid = 0;
        if (vfs_file_stat(path, &fmode, &fuid, &fgid) == 0 && (fmode & 04000U)) {
            proc->cred.euid = fuid;
            if (fuid == 0) proc->cred.egid = 0;
        }
    }

    /* Inherit pipe ends from parent into child at fd 0 (stdin) / fd 1 (stdout). */
    fd_table_t *parent_fds = current_fds();
    if (parent_fds) {
        if (stdin_fd_parent >= 0) {
            fd_type_t t = fd_type(parent_fds, (int)stdin_fd_parent);
            if (t == FD_PIPE_READ)
                fd_set(&proc->fds, 0, FD_PIPE_READ,
                       fd_id(parent_fds, (int)stdin_fd_parent));
        }
        if (stdout_fd_parent >= 0) {
            fd_type_t t = fd_type(parent_fds, (int)stdout_fd_parent);
            if (t == FD_PIPE_WRITE)
                fd_set(&proc->fds, 1, FD_PIPE_WRITE,
                       fd_id(parent_fds, (int)stdout_fd_parent));
        }
    }
    return (int64_t)proc->pid;
}

static int64_t sys_chown(uint64_t path_virt, uint64_t uid, uint64_t gid)
{
    if (path_virt >= 0x800000000000ULL) return -14;
    struct thread *t = sched_current();
    struct process *p = t ? (struct process *)t->process : NULL;
    if (!p || p->cred.euid != 0) return -1; /* EPERM */
    return vfs_chown((const char *)(uintptr_t)path_virt, (uint32_t)uid, (uint32_t)gid);
}

static int64_t sys_setuid(uint64_t uid)
{
    struct thread *t = sched_current();
    struct process *p = t ? (struct process *)t->process : NULL;
    if (!p) return -1;
    if (p->cred.euid != 0 && p->cred.uid != (uint32_t)uid) return -1; /* EPERM */
    p->cred.uid = p->cred.euid = (uint32_t)uid;
    return 0;
}

static int64_t sys_setgid(uint64_t gid)
{
    struct thread *t = sched_current();
    struct process *p = t ? (struct process *)t->process : NULL;
    if (!p) return -1;
    if (p->cred.egid != 0 && p->cred.gid != (uint32_t)gid) return -1;
    p->cred.gid = p->cred.egid = (uint32_t)gid;
    return 0;
}

/* sys_spawn_as: root-only; spawns child with specified uid/gid.
   Login uses this to start the shell as the logged-in user without
   dropping its own privileges. */
static int64_t sys_spawn_as(uint64_t path_virt, uint64_t argv_virt,
                             uint64_t child_uid, uint64_t child_gid)
{
    struct thread *ct = sched_current();
    struct process *caller = ct ? (struct process *)ct->process : NULL;
    if (!caller || caller->cred.euid != 0) return -1; /* EPERM */

    int64_t pid = sys_spawn(path_virt, argv_virt, -1, -1);
    if (pid < 0) return pid;

    struct process *child = process_find((uint32_t)pid);
    if (child) {
        child->cred.uid  = child->cred.euid  = (uint32_t)child_uid;
        child->cred.gid  = child->cred.egid  = (uint32_t)child_gid;
    }
    return pid;
}

static int64_t sys_pipe(uint64_t pipefd_virt)
{
    if (pipefd_virt >= 0x800000000000ULL) return -14;
    int32_t *pipefd = (int32_t *)(uintptr_t)pipefd_virt;
    fd_table_t *fds = current_fds();
    if (!fds) return -9;
    int pidx = pipe_alloc();
    if (pidx < 0) return -24;
    int rfd = fd_alloc_pipe(fds, pidx, FD_PIPE_READ);
    if (rfd < 0) { pipe_free(pidx); return -24; }
    int wfd = fd_alloc_pipe(fds, pidx, FD_PIPE_WRITE);
    if (wfd < 0) { fd_free(fds, rfd); pipe_free(pidx); return -24; }
    pipefd[0] = rfd;
    pipefd[1] = wfd;
    return 0;
}

static int64_t sys_listdir(uint64_t path_virt, uint64_t buf_virt,
                           uint64_t bufsz, uint64_t flags)
{
    if (path_virt >= 0x800000000000ULL || buf_virt >= 0x800000000000ULL) return -14;
    return (int64_t)vfs_listdir((const char *)(uintptr_t)path_virt,
                                 (char *)(uintptr_t)buf_virt,
                                 (uint32_t)bufsz, (uint32_t)flags);
}

static int64_t sys_stat(uint64_t path_virt, uint64_t buf_virt)
{
    if (path_virt >= 0x800000000000ULL || buf_virt >= 0x800000000000ULL) return -14;
    const char *path = (const char *)(uintptr_t)path_virt;
    /* Layout must match user-space struct stat in sys/stat.h */
    struct {
        uint16_t mode;
        uint16_t nlink;
        uint32_t uid;
        uint32_t gid;
        uint32_t size;
        uint32_t mtime;
    } __attribute__((packed)) *ubuf = (void *)(uintptr_t)buf_virt;
    ext2_stat_t st;
    if (vfs_stat(path, &st) < 0) return -2; /* ENOENT */
    ubuf->mode  = st.mode;
    ubuf->nlink = st.nlink;
    ubuf->uid   = st.uid;
    ubuf->gid   = st.gid;
    ubuf->size  = st.size;
    ubuf->mtime = st.mtime;
    return 0;
}

static int64_t sys_creat(uint64_t path_virt)
{
    if (path_virt >= 0x800000000000ULL) return -14;
    return vfs_creat((const char *)(uintptr_t)path_virt);
}

static int64_t sys_mkdir(uint64_t path_virt)
{
    if (path_virt >= 0x800000000000ULL) return -14;
    return vfs_mkdir((const char *)(uintptr_t)path_virt);
}

static int64_t sys_unlink(uint64_t path_virt)
{
    if (path_virt >= 0x800000000000ULL) return -14;
    return vfs_unlink((const char *)(uintptr_t)path_virt);
}

static int64_t sys_chdir(uint64_t path_virt)
{
    if (path_virt >= 0x800000000000ULL) return -14;
    return vfs_chdir((const char *)(uintptr_t)path_virt);
}

static int64_t sys_getcwd(uint64_t buf_virt, uint64_t size)
{
    if (buf_virt >= 0x800000000000ULL) return -14;
    if (vfs_getcwd((char *)(uintptr_t)buf_virt, (uint32_t)size) == 0)
        return (int64_t)strlen((char *)(uintptr_t)buf_virt);
    return -1;
}

/* ---- Socket syscalls ------------------------------------------------------- */

static int64_t sys_socket(uint64_t domain, uint64_t type, uint64_t proto)
{
    (void)domain; (void)type; (void)proto; /* TCP only */
    fd_table_t *fds = current_fds();
    if (!fds) return -9;
    int idx = sock_alloc();
    if (idx < 0) return -24; /* EMFILE */
    int fd = fd_alloc_socket(fds, idx);
    if (fd < 0) { sock_free(idx); return -24; }
    return fd;
}

static int64_t sys_connect(uint64_t fd, uint64_t addr_virt, uint64_t addrlen)
{
    (void)addrlen;
    if (addr_virt >= 0x800000000000ULL) return -14;
    fd_table_t *fds = current_fds();
    if (!fds) return -9;
    if (fd_type(fds, (int)fd) != FD_SOCKET) return -9;
    int idx = fd_id(fds, (int)fd);

    const struct sockaddr_in *sa = (const struct sockaddr_in *)(uintptr_t)addr_virt;
    uint16_t port = (uint16_t)((sa->sin_port >> 8) | (sa->sin_port << 8)); /* ntohs */
    return (int64_t)sock_connect(idx, sa->sin_addr, port);
}

static int64_t sys_send(uint64_t fd, uint64_t buf_virt, uint64_t len, uint64_t flags)
{
    (void)flags;
    if (buf_virt + len < buf_virt || buf_virt + len > 0x800000000000ULL) return -14;
    fd_table_t *fds = current_fds();
    if (!fds) return -9;
    if (fd_type(fds, (int)fd) != FD_SOCKET) return -9;
    int idx = fd_id(fds, (int)fd);
    return (int64_t)sock_send(idx, (const void *)(uintptr_t)buf_virt, (uint32_t)len);
}

static int64_t sys_recv(uint64_t fd, uint64_t buf_virt, uint64_t len, uint64_t flags)
{
    (void)flags;
    if (buf_virt + len < buf_virt || buf_virt + len > 0x800000000000ULL) return -14;
    fd_table_t *fds = current_fds();
    if (!fds) return -9;
    if (fd_type(fds, (int)fd) != FD_SOCKET) return -9;
    int idx = fd_id(fds, (int)fd);
    void *buf = (void *)(uintptr_t)buf_virt;

    int n;
    while ((n = sock_recv(idx, buf, (uint32_t)len)) == 0)
        __asm__ volatile("int $0x20" ::: "memory");
    if (n < 0) return 0; /* EOF */
    return (int64_t)n;
}

static int64_t sys_dns(uint64_t host_virt, uint64_t ip_out_virt)
{
    if (host_virt >= 0x800000000000ULL || ip_out_virt >= 0x800000000000ULL) return -14;
    const char *host = (const char *)(uintptr_t)host_virt;
    uint32_t *ip_out = (uint32_t *)(uintptr_t)ip_out_virt;
    uint32_t ip = 0;
    if (!dns_resolve(host, &ip)) return -1;
    *ip_out = ip;
    return 0;
}

int64_t syscall_dispatch(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4)
{
    (void)a4;
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
    case SYS_CHOWN:     return sys_chown(a0, a1, a2);
    case SYS_SETUID:    return sys_setuid(a0);
    case SYS_SETGID:    return sys_setgid(a0);
    case SYS_PIPE:      return sys_pipe(a0);
    case SYS_SPAWN:     return sys_spawn(a0, a1, (int64_t)a2, (int64_t)a3);
    case SYS_SPAWN_AS:  return sys_spawn_as(a0, a1, a2, a3);
    case SYS_STAT:    return sys_stat(a0, a1);
    case SYS_LISTDIR: return sys_listdir(a0, a1, a2, a3);
    case SYS_CREAT:   return sys_creat(a0);
    case SYS_MKDIR:   return sys_mkdir(a0);
    case SYS_UNLINK:  return sys_unlink(a0);
    case SYS_RENAME: {
        if (a0 >= 0x800000000000ULL || a1 >= 0x800000000000ULL) return -14;
        return vfs_rename((const char *)(uintptr_t)a0, (const char *)(uintptr_t)a1);
    }
    case SYS_CHDIR:   return sys_chdir(a0);
    case SYS_GETCWD:  return sys_getcwd(a0, a1);
    case SYS_SOCKET:  return sys_socket(a0, a1, a2);
    case SYS_CONNECT: return sys_connect(a0, a1, a2);
    case SYS_SEND:    return sys_send(a0, a1, a2, a3);
    case SYS_RECV:    return sys_recv(a0, a1, a2, a3);
    case SYS_DNS:     return sys_dns(a0, a1);
    case SYS_CHMOD: {
        if (a0 >= 0x800000000000ULL) return -14;
        struct process *p = sched_current_process();
        if (!p) return -1;
        uint16_t fmode = 0; uint32_t fuid = 0, fgid = 0;
        if (vfs_file_stat((const char *)(uintptr_t)a0, &fmode, &fuid, &fgid) < 0) return -2;
        if (p->cred.euid != 0 && p->cred.euid != fuid) return -1; /* EPERM */
        return vfs_chmod((const char *)(uintptr_t)a0, (uint16_t)(a1 & 0xfffU));
    }
    case SYS_SLEEP:   sched_sleep_ms(a0); return 0;
    case SYS_KILL: {
        struct process *caller = sched_current_process();
        if (!caller) return -1;
        struct process *victim = process_find((uint32_t)a0);
        if (!victim) return -3; /* ESRCH */
        if (caller->cred.euid != 0 && caller->cred.uid != victim->cred.uid) return -1;
        process_kill(victim);
        return 0;
    }
    case SYS_PS: {
        if (a0 >= 0x800000000000ULL) return -14;
        process_ps((char *)(uintptr_t)a0, (uint32_t)a1);
        return 0;
    }
    case 169: {   /* sys_reboot — root-only */
        struct process *p = sched_current_process();
        if (!p || p->cred.uid != 0) return -1;
        if (a0 == 0) acpi_poweroff();
        else         acpi_reboot();
        return 0;
    }
    default:
        serial_write("syscall: unknown nr=");
        serial_write_dec(nr);
        serial_write("\n");
        return -38;   /* -ENOSYS */
    }
}
