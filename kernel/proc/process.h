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
    struct thread *thread;
    uint64_t      cr3;
    fd_table_t    fds;
    cred_t        cred;
    volatile bool    exited;
    volatile int32_t exit_status;
    struct thread   *wait_queue;  /* threads blocked in sys_waitpid */
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
void process_wait(struct process *proc);

/* Mark `proc` exited with `status` and wake all waiters (lock-protected). */
void process_mark_exited(struct process *proc, int32_t status);

/* Load an ELF binary and create a user-mode thread. Returns NULL on failure. */
struct process *process_create_from_elf(const void *elf_data, uint64_t size);

/* Look up a process by PID. Returns NULL if not found. */
struct process *process_find(uint32_t pid);

/* Free an exited process's struct and release its table slot. */
void process_reap(struct process *proc);

/* Forcibly terminate a process (SIGKILL semantics). Safe to call from kernel. */
void process_kill(struct process *proc);

/* Fill buf with a human-readable process table (null-terminated).
   Returns the number of bytes written, including the NUL terminator. */
uint32_t process_ps(char *buf, uint32_t bufsz);

#endif
