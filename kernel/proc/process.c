#include "proc/process.h"

#include <stddef.h>
#include <stdint.h>

#include "core/serial.h"
#include "exec/elf.h"
#include "lib/string.h"
#include "mem/heap.h"
#include "proc/fd.h"
#include "proc/pipe.h"
#include "sched/sched.h"
#include "sched/thread.h"
#include "security/cred.h"
#include "sync/spinlock.h"

#define MAX_PROCS 64
static struct process *s_procs[MAX_PROCS];
static uint32_t next_pid = 1;

/* Protects s_procs[] and next_pid against concurrent spawn/reap/find on SMP. */
static spinlock_t proc_table_lock = SPINLOCK_INIT;

/* Protects every process's wait_queue and exited flag.  Acquired by waiters
   (process_wait) and by exit notifiers (process_mark_exited) so a wakeup can
   never be lost between a waiter's exited-check and its enqueue. */
static spinlock_t proc_wait_lock = SPINLOCK_INIT;

static struct process *find_locked(uint32_t pid)
{
    for (int i = 0; i < MAX_PROCS; ++i)
        if (s_procs[i] && s_procs[i]->pid == pid) return s_procs[i];
    return NULL;
}

struct process *process_find(uint32_t pid)
{
    uint64_t f = spinlock_acquire_irqsave(&proc_table_lock);
    struct process *p = find_locked(pid);
    spinlock_release_irqrestore(&proc_table_lock, f);
    return p;
}

/* Allocate a pid not currently in use. next_pid advances monotonically but
   wraps and skips live pids, so freed pids are eventually reused instead of
   leaking the id space. Bounded: at most MAX_PROCS are live.
   Caller must hold proc_table_lock. */
static uint32_t alloc_pid_locked(void)
{
    for (int i = 0; i < 2 * MAX_PROCS + 2; ++i) {
        uint32_t cand = next_pid++;
        if (next_pid == 0) next_pid = 1;          /* skip 0 on wrap */
        if (cand != 0 && !find_locked(cand)) return cand;
    }
    return next_pid++; /* unreachable in practice (only MAX_PROCS live) */
}

/* Free an exited process's struct and release its table slot. Caller must
   have already collected exit_status (e.g. from sys_waitpid). The associated
   thread struct is freed independently by the scheduler's deferred reaper. */
void process_reap(struct process *proc)
{
    if (!proc) return;
    uint64_t f = spinlock_acquire_irqsave(&proc_table_lock);
    for (int i = 0; i < MAX_PROCS; ++i) {
        if (s_procs[i] == proc) { s_procs[i] = NULL; break; }
    }
    spinlock_release_irqrestore(&proc_table_lock, f);
    kfree(proc);
}

/* Block the current thread until `target` exits.  Lock-protected so the wakeup
   from process_mark_exited cannot be lost between the exited-check and the
   enqueue (a BLOCKED thread is never re-queued, so a lost wake hangs forever). */
void process_wait(struct process *target)
{
    struct thread *t = sched_current();
    spinlock_acquire(&proc_wait_lock);
    while (!target->exited) {
        t->wait_next       = target->wait_queue;
        target->wait_queue = t;
        t->state           = THREAD_BLOCKED;
        spinlock_release(&proc_wait_lock);
        /* cli before int $0x20 so the timer cannot preempt between marking
           BLOCKED and yielding; iretq restores IF, then re-enable. */
        __asm__ volatile("cli; int $0x20; sti" ::: "memory");
        spinlock_acquire(&proc_wait_lock);
    }
    spinlock_release(&proc_wait_lock);
}

/* Mark a process exited and wake all sys_waitpid waiters.  Setting `exited`
   and draining wait_queue under proc_wait_lock closes the lost-wakeup race. */
void process_mark_exited(struct process *proc, int32_t status)
{
    spinlock_acquire(&proc_wait_lock);
    proc->exit_status = status;
    proc->exited      = true;
    struct thread *w  = proc->wait_queue;
    proc->wait_queue  = NULL;
    spinlock_release(&proc_wait_lock);

    while (w) {
        struct thread *nxt = w->wait_next;
        w->wait_next = NULL;
        sched_wake(w);
        w = nxt;
    }
}

/* Make a fully-initialized process runnable.  Called only after credentials,
   fds, and name are set, so the thread never runs with stale state. */
void process_start(struct process *proc)
{
    if (proc && proc->thread) sched_add(proc->thread);
}

struct process *process_create_from_result(const elf_load_result_t *r)
{
    struct process *proc = (struct process *)kmalloc(sizeof(struct process));
    if (proc == NULL) {
        serial_write("process: kmalloc failed\n");
        return NULL;
    }

    proc->cr3         = r->cr3;
    proc->exited      = false;
    proc->exit_status = 0;
    proc->wait_queue  = NULL;
    proc->name[0]     = '\0';
    struct process *parent = sched_current_process();
    if (parent && parent->cwd[0]) {
        strncpy(proc->cwd, parent->cwd, sizeof(proc->cwd) - 1);
        proc->cwd[sizeof(proc->cwd) - 1] = '\0';
    } else {
        proc->cwd[0] = '/'; proc->cwd[1] = '\0';
    }
    fd_table_init(&proc->fds);
    cred_init(&proc->cred, 0, 0);
    proc->thread = thread_create_user(r->entry, r->user_sp, r->cr3);
    proc->thread->process = proc;

    /* Assign the pid and publish into the table atomically. */
    uint64_t f = spinlock_acquire_irqsave(&proc_table_lock);
    proc->pid = alloc_pid_locked();
    for (int i = 0; i < MAX_PROCS; ++i) {
        if (!s_procs[i]) { s_procs[i] = proc; break; }
    }
    spinlock_release_irqrestore(&proc_table_lock, f);

    serial_write("process: created pid=");
    serial_write_dec(proc->pid);
    serial_write(" entry=");
    serial_write_hex(r->entry);
    serial_write("\n");

    return proc;
}

void process_kill(struct process *proc)
{
    if (!proc || proc->exited) return;
    /* Close all pipe fds so connected processes see EOF. */
    for (int i = 0; i < MAX_FDS; i++) {
        if (!proc->fds.fds[i].open) continue;
        fd_type_t ft = proc->fds.fds[i].type;
        if (ft == FD_PIPE_READ)  pipe_close_read(proc->fds.fds[i].id);
        if (ft == FD_PIPE_WRITE) pipe_close_write(proc->fds.fds[i].id);
        proc->fds.fds[i].open = false;
    }
    /* Record exit status and wake any sys_waitpid callers (lock-protected). */
    process_mark_exited(proc, 130);   /* 128 + SIGINT */
    /* Mark the process thread as dead. */
    if (proc->thread) proc->thread->state = THREAD_DEAD;
}

static uint32_t ps_fmt_uint(char *out, uint32_t val, uint32_t width)
{
    char tmp[12];
    uint32_t len = 0;
    if (val == 0) { tmp[len++] = '0'; }
    else { while (val) { tmp[len++] = (char)('0' + val % 10); val /= 10; } }
    /* reverse */
    for (uint32_t i = 0, j = len - 1; i < j; i++, j--) {
        char c = tmp[i]; tmp[i] = tmp[j]; tmp[j] = c;
    }
    uint32_t off = 0;
    while (off + len < width) out[off++] = ' ';
    for (uint32_t k = 0; k < len; k++) out[off++] = tmp[k];
    return off;
}

uint32_t process_ps(char *buf, uint32_t bufsz)
{
    if (bufsz == 0) return 0;
    static const char hdr[] = "  PID   UID  NAME\n";
    uint32_t off = 0;
    for (uint32_t i = 0; hdr[i] && off + 1 < bufsz; i++)
        buf[off++] = hdr[i];

    for (int i = 0; i < MAX_PROCS; ++i) {
        struct process *p = s_procs[i];
        if (!p || p->exited) continue;
        if (off + 48 >= bufsz) break;

        off += ps_fmt_uint(buf + off, p->pid, 5);
        buf[off++] = ' ';
        off += ps_fmt_uint(buf + off, p->cred.uid, 5);
        buf[off++] = ' ';
        buf[off++] = ' ';

        const char *nm = p->name[0] ? p->name : "?";
        for (uint32_t k = 0; nm[k] && off + 1 < bufsz; k++)
            buf[off++] = nm[k];
        buf[off++] = '\n';
    }
    /* NUL-terminate and return the number of valid bytes (including the
       terminator).  The caller must copy only this many bytes to userspace so
       it never exposes the uninitialized remainder of the heap buffer. */
    if (off < bufsz) {
        buf[off] = '\0';
        return off + 1;
    }
    buf[bufsz - 1] = '\0';
    return bufsz;
}

struct process *process_create_from_elf(const void *elf_data, uint64_t size)
{
    elf_load_result_t result;
    if (!elf_load(elf_data, size, &result)) {
        serial_write("process: elf_load failed\n");
        return NULL;
    }
    struct process *proc = process_create_from_result(&result);
    if (proc) process_start(proc);
    return proc;
}
