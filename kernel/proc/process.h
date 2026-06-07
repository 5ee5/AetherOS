#ifndef KERNEL_PROC_PROCESS_H
#define KERNEL_PROC_PROCESS_H

#include <stdbool.h>
#include <stdint.h>

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
};

/* Load an ELF binary and create a user-mode thread. Returns NULL on failure. */
struct process *process_create_from_elf(const void *elf_data, uint64_t size);

/* Look up a process by PID. Returns NULL if not found. */
struct process *process_find(uint32_t pid);

#endif
