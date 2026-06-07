#include "proc/process.h"

#include <stddef.h>
#include <stdint.h>

#include "core/serial.h"
#include "exec/elf.h"
#include "mem/heap.h"
#include "proc/fd.h"
#include "sched/thread.h"
#include "security/cred.h"

static uint32_t next_pid = 1;

struct process *process_create_from_elf(const void *elf_data, uint64_t size)
{
    elf_load_result_t result;
    if (!elf_load(elf_data, size, &result)) {
        serial_write("process: elf_load failed\n");
        return NULL;
    }

    struct process *proc = (struct process *)kmalloc(sizeof(struct process));
    if (proc == NULL) {
        serial_write("process: kmalloc failed\n");
        return NULL;
    }

    proc->pid    = next_pid++;
    proc->cr3    = result.cr3;
    fd_table_init(&proc->fds);
    cred_init(&proc->cred, 1000, 1000); /* unprivileged user */
    proc->thread = thread_create_user(result.entry, result.user_sp, result.cr3);
    proc->thread->process = proc;

    serial_write("process: created pid=");
    serial_write_dec(proc->pid);
    serial_write(" entry=");
    serial_write_hex(result.entry);
    serial_write("\n");

    return proc;
}
