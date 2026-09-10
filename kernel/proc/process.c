#include "proc/process.h"

#include <stddef.h>
#include <stdint.h>

#include "core/serial.h"
#include "exec/elf.h"
#include "fs/vfs.h"
#include "lib/string.h"
#include "mem/heap.h"
#include "mem/vmm.h"
#include "net/socket.h"
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

struct process *process_find_ref(uint32_t pid)
{
    uint64_t f = spinlock_acquire_irqsave(&proc_table_lock);
    struct process *p = find_locked(pid);
    if (p) p->refs++;
    spinlock_release_irqrestore(&proc_table_lock, f);
    return p;
}

void process_put(struct process *proc)
{
    if (!proc) return;
    bool freeit = false;
    uint64_t f = spinlock_acquire_irqsave(&proc_table_lock);
    if (proc->refs > 0) proc->refs--;
    /* Only the reaper that claimed the table slot sets `reaped`, so exactly one
       path frees, and it is whichever of the two finishes last. */
    if (proc->refs == 0 && proc->reaped) freeit = true;
    spinlock_release_irqrestore(&proc_table_lock, f);
    if (freeit) kfree(proc);
}

bool process_exists(uint32_t pid)
{
    uint64_t f = spinlock_acquire_irqsave(&proc_table_lock);
    bool found = (find_locked(pid) != NULL);
    spinlock_release_irqrestore(&proc_table_lock, f);
    return found;
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
    /* Claim the table slot under the lock and free only if we won.  Two callers
       racing on the same process would otherwise both reach kfree. */
    bool claimed = false, freeit = false;
    uint64_t f = spinlock_acquire_irqsave(&proc_table_lock);
    for (int i = 0; i < MAX_PROCS; ++i) {
        if (s_procs[i] == proc) {
            s_procs[i] = NULL;
            claimed = true;
            proc->reaped = true;
            break;
        }
    }
    /* Defer the free while anyone still holds a reference from
       process_find_ref; their process_put does it instead. */
    if (claimed && proc->refs == 0) freeit = true;
    spinlock_release_irqrestore(&proc_table_lock, f);
    if (freeit) kfree(proc);
}

/* Called when `parent` exits.  Any child of its that is already a zombie will
   never be collected, so free it now; any child still running is re-parented to
   0 (no parent) so it self-reaps on exit instead of sitting in the table
   forever.  Without this a shell that launched background jobs left one dead
   entry per job, and MAX_PROCS entries in, nothing could spawn again. */
void process_orphan_children(struct process *parent)
{
    if (!parent) return;
    for (;;) {
        struct process *zombie = NULL;
        uint64_t f = spinlock_acquire_irqsave(&proc_table_lock);
        for (int i = 0; i < MAX_PROCS; ++i) {
            struct process *c = s_procs[i];
            if (!c || c->ppid != parent->pid || c == parent) continue;
            if (c->exited) { zombie = c; break; }
            c->ppid = 0;    /* orphan: reaped by the scheduler when it dies */
        }
        spinlock_release_irqrestore(&proc_table_lock, f);
        if (!zombie) return;
        /* Outside the lock: process_reap takes it and kfree may too. */
        process_reap(zombie);
    }
}

/* A process whose parent is gone can never be collected by waitpid — sys_waitpid
   only accepts the recorded parent — so it is safe to free unilaterally.  Called
   from the scheduler's deferred reaper, once the thread is off its own stack. */
void process_reap_if_orphaned(struct process *proc)
{
    if (!proc || !proc->exited) return;
    if (proc->ppid != 0 && process_exists(proc->ppid)) return;
    process_reap(proc);
}

/* Remove t from target's wait queue if it is still linked there.
   Caller holds proc_wait_lock. */
static void waitq_remove_locked(struct process *target, struct thread *t)
{
    struct thread **p = &target->wait_queue;
    while (*p) {
        if (*p == t) {
            *p = t->wait_next;
            t->wait_next = NULL;
            return;
        }
        p = &(*p)->wait_next;
    }
}

/* Block the current thread until `target` exits or `ms` elapses; returns whether
   it has exited.  Returning periodically is the point: sys_waitpid has to watch
   the console for Ctrl+C, and a parent blocked until its child exits can never
   look — so the check ran exactly once per spawn and Ctrl+C did nothing.

   Enqueueing on the wait queue *and* taking a sleep deadline means two wakeup
   sources race, which is safe because sched_wake and the sleeper loop both
   promote only a BLOCKED thread, so the loser cannot enqueue a second time.
   Registering on the queue before sleeping also keeps the wakeup from being lost
   between the exited-check and the sleep. */
bool process_wait_timeout(struct process *target, uint64_t ms)
{
    struct thread *t = sched_current();

    spinlock_acquire(&proc_wait_lock);
    if (target->exited) {
        spinlock_release(&proc_wait_lock);
        return true;
    }
    t->wait_next       = target->wait_queue;
    target->wait_queue = t;
    /* Tells sched_kill that sleep-list membership alone does not make this thread
       safe to free while we are also linked here. */
    t->on_waitq        = true;
    spinlock_release(&proc_wait_lock);

    sched_sleep_ms(ms);

    spinlock_acquire(&proc_wait_lock);
    /* Unlink unless process_mark_exited already drained us.  wait_next is shared
       with the mutex wait list, so leaving a stale link here would let the next
       mutex_lock overwrite it and corrupt this queue. */
    waitq_remove_locked(target, t);
    t->on_waitq = false;
    bool exited = target->exited;
    spinlock_release(&proc_wait_lock);
    return exited;
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
        /* No longer linked here, so sleep-list membership alone is once again
           enough for sched_kill to free it. */
        w->on_waitq  = false;
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
        vmm_space_destroy(r->cr3);
        return NULL;
    }

    proc->cr3         = r->cr3;
    proc->exited      = false;
    proc->refs        = 0;
    proc->reaped      = false;
    proc->exit_status = 0;
    proc->wait_queue  = NULL;
    proc->in_waitpid      = false;
    proc->in_console_read = false;
    proc->name[0]     = '\0';
    struct process *parent = sched_current_process();
    proc->ppid        = parent ? parent->pid : 0;   /* 0 = spawned by the kernel */
    if (parent && parent->cwd[0]) {
        strncpy(proc->cwd, parent->cwd, sizeof(proc->cwd) - 1);
        proc->cwd[sizeof(proc->cwd) - 1] = '\0';
    } else {
        proc->cwd[0] = '/'; proc->cwd[1] = '\0';
    }
    fd_table_init(&proc->fds);
    cred_init(&proc->cred, 0, 0);
    proc->thread = thread_create_user(r->entry, r->user_sp, r->cr3);
    if (proc->thread == NULL) {
        serial_write("process: no memory for a thread\n");
        vmm_space_destroy(r->cr3);
        kfree(proc);
        return NULL;
    }
    proc->thread->process = proc;

    /* Assign the pid and publish into the table atomically.  Publishing must
       succeed: a process that was started without a table slot was invisible to
       ps, could never be waited on, and was never reaped — so refuse to create
       it at all rather than leaving a running orphan behind. */
    bool published = false;
    uint64_t f = spinlock_acquire_irqsave(&proc_table_lock);
    proc->pid = alloc_pid_locked();
    for (int i = 0; i < MAX_PROCS; ++i) {
        if (!s_procs[i]) { s_procs[i] = proc; published = true; break; }
    }
    spinlock_release_irqrestore(&proc_table_lock, f);

    if (!published) {
        serial_write("process: table full\n");
        /* The thread was never made runnable, so nothing else can reach it. */
        vmm_space_destroy(r->cr3);
        kfree(proc->thread->kstack);
        kfree(proc->thread);
        kfree(proc);
        return NULL;
    }

    serial_write("process: created pid=");
    serial_write_dec(proc->pid);
    serial_write(" entry=");
    serial_write_hex(r->entry);
    serial_write("\n");

    return proc;
}

/* Release every descriptor the process holds.  Pipe ends propagate EOF to the
   other side; file and socket slots return to their global tables.  Those
   tables are small fixed arrays (16 open files, 8 sockets), so a process that
   exited without closing used to burn a slot permanently — sixteen such exits
   and nothing on the system could open a file again. */
void process_close_all_fds(struct process *proc)
{
    if (!proc) return;
    for (int i = 0; i < MAX_FDS; i++) {
        if (!proc->fds.fds[i].open) continue;
        int id = proc->fds.fds[i].id;
        switch (proc->fds.fds[i].type) {
        case FD_PIPE_READ:  pipe_close_read(id);  break;
        case FD_PIPE_WRITE: pipe_close_write(id); break;
        case FD_SOCKET:     sock_close(id);       break;
        case FD_FILE:       vfs_close(id);        break;
        default: break;
        }
        proc->fds.fds[i].open = false;
    }
}

void process_kill(struct process *proc)
{
    if (!proc || proc->exited) return;

    /* Killing ourselves -- a user fault (idt.c) or kill(getpid()) -- must tear the
       process down BEFORE the thread is marked dead, exactly as sys_exit does.
       sched_kill sets THREAD_DEAD on the *running* thread and returns true, and
       from that instant the next timer tick hands it to the reaper and never
       resumes it.  Everything after the call would then be abandoned mid-flight:
       process_close_all_fds takes vfs_lock and does block I/O, so a segfault timed
       inside it left the filesystem mutex held forever, wedging every other
       process on the machine.  Do the teardown while still runnable, then die. */
    if (proc->thread && proc->thread == sched_current()) {
        struct thread *self = proc->thread;
        process_close_all_fds(proc);
        process_orphan_children(proc);
        proc->thread = NULL;
        process_mark_exited(proc, 130);   /* 128 + SIGINT */
        process_reap_if_orphaned(proc);

        /* The address space is freed by the scheduler reaper once we are off our
           own CR3; destroying it here would unmap the page tables still in CR3. */
        __asm__ volatile("cli" ::: "memory");
        self->state = THREAD_DEAD;
        SCHED_YIELD();
        for (;;) __asm__ volatile("hlt");   /* not reached */
    }

    /* If the victim is inside the kernel holding a lock, sched_kill only flags it
       and it dies at its next syscall return.  Leave the process completely alone
       in that case: publishing the exit status here would let the parent's waitpid
       reap and free this struct while the thread is still running with a pointer
       to it.  The thread performs the whole teardown itself when it unwinds. */
    if (proc->thread && !sched_kill(proc->thread)) return;

    process_close_all_fds(proc);
    /* The thread is already dead (sched_kill above ran before anything else, so
       the parent's waitpid could not free this struct under a live thread).
       Forget it: sched_kill may have handed it straight to the reaper, so the
       pointer can dangle from here on, and clearing it makes a second racing
       process_kill a no-op rather than a use-after-free. */
    proc->thread = NULL;
    /* Record exit status and wake any sys_waitpid callers (lock-protected). */
    process_mark_exited(proc, 130);   /* 128 + SIGINT */
    /* Nothing may ever waitpid an orphan, so collect it here while `proc` is
       still known to be live.  The thread reaper used to do this from
       t->process, which the parent's waitpid could have freed first. */
    process_reap_if_orphaned(proc);
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

    /* Hold the table lock across the walk: without it this read a struct that
       another CPU was freeing in process_reap.  Formatting into a kernel buffer
       takes no further locks and is bounded by MAX_PROCS, so the critical
       section stays short. */
    uint64_t f = spinlock_acquire_irqsave(&proc_table_lock);
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
    spinlock_release_irqrestore(&proc_table_lock, f);
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
