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

void process_ps(char *buf, uint32_t bufsz)
{
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
    if (off < bufsz) buf[off] = '\0';
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
