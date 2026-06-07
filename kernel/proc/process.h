#ifndef KERNEL_PROC_PROCESS_H
#define KERNEL_PROC_PROCESS_H

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
};

/* Load an ELF binary and create a user-mode thread. Returns NULL on failure. */
struct process *process_create_from_elf(const void *elf_data, uint64_t size);

#endif
