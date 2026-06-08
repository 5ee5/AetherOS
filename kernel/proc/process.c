#include "proc/process.h"

#include <stddef.h>
#include <stdint.h>

#include "core/serial.h"
#include "exec/elf.h"
#include "mem/heap.h"
#include "proc/fd.h"
#include "sched/thread.h"
#include "security/cred.h"

#define MAX_PROCS 16
static struct process *s_procs[MAX_PROCS];
static uint32_t next_pid = 1;

struct process *process_find(uint32_t pid)
{
    for (int i = 0; i < MAX_PROCS; ++i)
        if (s_procs[i] && s_procs[i]->pid == pid) return s_procs[i];
    return NULL;
}

struct process *process_create_from_result(const elf_load_result_t *r)
{
    struct process *proc = (struct process *)kmalloc(sizeof(struct process));
    if (proc == NULL) {
        serial_write("process: kmalloc failed\n");
        return NULL;
    }

    proc->pid         = next_pid++;
    proc->cr3         = r->cr3;
    proc->exited      = false;
    proc->exit_status = 0;
    proc->wait_queue  = NULL;
    proc->cwd[0] = '/'; proc->cwd[1] = '\0';
    fd_table_init(&proc->fds);
    cred_init(&proc->cred, 1000, 1000);
    proc->thread = thread_create_user(r->entry, r->user_sp, r->cr3);
    proc->thread->process = proc;

    for (int i = 0; i < MAX_PROCS; ++i) {
        if (!s_procs[i]) { s_procs[i] = proc; break; }
    }

    serial_write("process: created pid=");
    serial_write_dec(proc->pid);
    serial_write(" entry=");
    serial_write_hex(r->entry);
    serial_write("\n");

    return proc;
}

struct process *process_create_from_elf(const void *elf_data, uint64_t size)
{
    elf_load_result_t result;
    if (!elf_load(elf_data, size, &result)) {
        serial_write("process: elf_load failed\n");
        return NULL;
    }
    return process_create_from_result(&result);
}
