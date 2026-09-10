#ifndef KERNEL_PROC_PROCESS_H
#define KERNEL_PROC_PROCESS_H

#include <stdbool.h>
#include <stdint.h>

#include "exec/elf.h"
#include "proc/fd.h"
#include "security/cred.h"

struct thread;

struct process {
    uint32_t      pid;
    /* pid of the spawning process, or 0 for processes created directly by the
       kernel (e.g. /bin/login).  sys_waitpid only accepts a caller's own
       children, so this is what stops one user waiting on — and, via the
       Ctrl+C path, killing or reaping — an unrelated process. */
    uint32_t      ppid;
    struct thread *thread;
    uint64_t      cr3;
    fd_table_t    fds;
    cred_t        cred;
    volatile bool    exited;
    volatile int32_t exit_status;
    /* Outstanding process_find_ref references, and whether the table slot has
       been released.  The struct is freed by whichever of process_reap and the
       last process_put runs last, so a lookup result can never be freed under a
       caller that is still holding it.  Both are protected by proc_table_lock. */
    uint32_t         refs;
    bool             reaped;
    struct thread   *wait_queue;  /* threads blocked in sys_waitpid */
    /* Which end of the console this process is currently sitting on.  Ctrl+C is
       polled by every sys_waitpid caller, so in a login -> shell -> job chain
       several processes watch the same console at once; without these, whichever
       waiter peeked first won and login could kill the shell instead of the job.
       A waiter only claims a Ctrl+C when its target is doing neither — i.e. the
       target is the foreground job rather than an intermediate shell. */
    volatile bool    in_waitpid;
    volatile bool    in_console_read;
    char             cwd[256];
    char             name[32];    /* basename of spawned ELF path */
};

/* Create a process from an already-loaded ELF result (entry/user_sp/cr3).
   The process thread is created parked; call process_start() to run it. */
struct process *process_create_from_result(const elf_load_result_t *r);

/* Make a fully-initialized process runnable (adds its thread to the run queue).
   Call only after credentials, fds, and name are set. */
void process_start(struct process *proc);

/* Block the current thread until `proc` exits (used by sys_waitpid). */
bool process_wait_timeout(struct process *proc, uint64_t ms);

/* Mark `proc` exited with `status` and wake all waiters (lock-protected). */
void process_mark_exited(struct process *proc, int32_t status);

/* Load an ELF binary and create a user-mode thread. Returns NULL on failure. */
struct process *process_create_from_elf(const void *elf_data, uint64_t size);

/* Look a process up by pid and take a reference, so the struct cannot be freed
   while the caller still holds the pointer.  Release it with process_put.

   This replaces a process_find that returned a raw pointer after dropping
   proc_table_lock: a parent's waitpid could reap and kfree the struct between
   the lookup and the caller's first dereference, a use-after-free reachable
   from kill(2) and made worse by pid reuse handing the same number to a
   different process. */
struct process *process_find_ref(uint32_t pid);

/* Drop a reference taken by process_find_ref, freeing the struct if it has
   already been reaped and this was the last reference. */
void process_put(struct process *proc);

/* Existence check that never hands out a pointer, for callers that only need to
   know whether a pid is still live. */
bool process_exists(uint32_t pid);

/* Free an exited process's struct and release its table slot.  Idempotent:
   only the caller that removes the table entry frees the struct, so a racing
   second reap of the same process is a no-op rather than a double free.  If
   references are outstanding the free is deferred to the last process_put. */
void process_reap(struct process *proc);

/* Forcibly terminate a process (SIGKILL semantics). Safe to call from kernel. */
void process_kill(struct process *proc);

/* Release every fd the process holds (pipes, files, sockets). */
void process_close_all_fds(struct process *proc);

/* Called when `parent` exits: collect its zombie children and orphan the rest
   so nothing is left waiting on a parent that will never call waitpid. */
void process_orphan_children(struct process *parent);

/* Reap `proc` if it has exited and no live parent can ever wait on it. */
void process_reap_if_orphaned(struct process *proc);

/* Fill buf with a human-readable process table (null-terminated).
   Returns the number of bytes written, including the NUL terminator. */
uint32_t process_ps(char *buf, uint32_t bufsz);

#endif
