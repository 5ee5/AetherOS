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
    proc->exit_status = 130;   /* 128 + SIGINT */
    proc->exited = true;
    /* Wake any sys_waitpid callers. */
    struct thread *w = proc->wait_queue;
    proc->wait_queue = NULL;
    while (w) {
        struct thread *nxt = w->wait_next;
        w->wait_next = NULL;
        sched_wake(w);
        w = nxt;
    }
    /* Mark the process thread as dead. */
    if (proc->thread) proc->thread->state = THREAD_DEAD;
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
