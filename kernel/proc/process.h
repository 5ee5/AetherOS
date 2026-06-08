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
};

/* Create a process from an already-loaded ELF result (entry/user_sp/cr3). */
struct process *process_create_from_result(const elf_load_result_t *r);

/* Load an ELF binary and create a user-mode thread. Returns NULL on failure. */
struct process *process_create_from_elf(const void *elf_data, uint64_t size);

/* Look up a process by PID. Returns NULL if not found. */
struct process *process_find(uint32_t pid);

/* Forcibly terminate a process (SIGKILL semantics). Safe to call from kernel. */
void process_kill(struct process *proc);

#endif
