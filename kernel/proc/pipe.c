#include "proc/pipe.h"
#include "lib/string.h"
#include "sync/spinlock.h"

#define PIPE_BUF  4096
#define MAX_PIPES 8

/* Each end is reference counted rather than flagged closed, because a pipe end
   is held by more than one process: the shell creates the pipe, hands an end to
   each stage of a pipeline, then closes its own copies.  With a single
   `closed` boolean the shell's close tore the end down under the children —
   the writer got EPIPE and the reader saw EOF before any data moved, so
   pipelines transferred nothing. */
typedef struct {
    bool     in_use;
    uint8_t  buf[PIPE_BUF];
    uint32_t head;          /* next read position  */
    uint32_t count;         /* bytes buffered      */
    uint32_t readers;       /* open read ends      */
    uint32_t writers;       /* open write ends     */
} pipe_t;

static pipe_t s_pipes[MAX_PIPES];
static spinlock_t pipe_lock = SPINLOCK_INIT;

int pipe_alloc(void)
{
    spinlock_acquire(&pipe_lock);
    for (int i = 0; i < MAX_PIPES; i++) {
        if (!s_pipes[i].in_use) {
            memset(&s_pipes[i], 0, sizeof(s_pipes[i]));
            s_pipes[i].in_use = true;
            /* The creator holds both ends; sys_pipe hands it two fds. */
            s_pipes[i].readers = 1;
            s_pipes[i].writers = 1;
            spinlock_release(&pipe_lock);
            return i;
        }
    }
    spinlock_release(&pipe_lock);
    return -1;
}

/* Claim an additional reference to an end, for a child inheriting it. Must be
   called before the child can run, while the parent still holds its own ref. */
void pipe_dup_read(int idx)
{
    if (idx < 0 || idx >= MAX_PIPES) return;
    spinlock_acquire(&pipe_lock);
    if (s_pipes[idx].in_use) s_pipes[idx].readers++;
    spinlock_release(&pipe_lock);
}

void pipe_dup_write(int idx)
{
    if (idx < 0 || idx >= MAX_PIPES) return;
    spinlock_acquire(&pipe_lock);
    if (s_pipes[idx].in_use) s_pipes[idx].writers++;
    spinlock_release(&pipe_lock);
}

void pipe_free(int idx)
{
    if (idx < 0 || idx >= MAX_PIPES) return;
    spinlock_acquire(&pipe_lock);
    s_pipes[idx].in_use = false;
    spinlock_release(&pipe_lock);
}

/* Returns bytes written (may be less than len if buffer full). */
int pipe_write(int idx, const void *buf, uint32_t len)
{
    if (idx < 0 || idx >= MAX_PIPES) return -1;
    spinlock_acquire(&pipe_lock);
    pipe_t *p = &s_pipes[idx];
    /* No reader left anywhere: the data has nowhere to go (EPIPE). */
    if (!p->in_use || p->readers == 0) { spinlock_release(&pipe_lock); return -1; }
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t written = 0;
    while (written < len && p->count < PIPE_BUF) {
        uint32_t wp = (p->head + p->count) % PIPE_BUF;
        p->buf[wp] = src[written++];
        p->count++;
    }
    spinlock_release(&pipe_lock);
    return (int)written;
}

/* Returns bytes read; 0 = EOF (write end closed and buffer empty);
   -2 = EAGAIN (no data yet, retry). */
int pipe_read(int idx, void *buf, uint32_t len)
{
    if (idx < 0 || idx >= MAX_PIPES) return -1;
    spinlock_acquire(&pipe_lock);
    pipe_t *p = &s_pipes[idx];
    if (!p->in_use) { spinlock_release(&pipe_lock); return -1; }
    if (p->count == 0) {
        /* EOF only once every writer is gone and the buffer is drained. */
        int rv = (p->writers == 0) ? 0 /* EOF */ : -2 /* no data yet */;
        spinlock_release(&pipe_lock);
        return rv;
    }
    uint8_t *dst = (uint8_t *)buf;
    uint32_t n = (len < p->count) ? len : p->count;
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = p->buf[p->head];
        p->head = (p->head + 1) % PIPE_BUF;
    }
    p->count -= n;
    spinlock_release(&pipe_lock);
    return (int)n;
}

/* Drop one reference to an end.  The slot is recycled only once both ends have
   no holders left, so a pipe outlives whichever process created it. */
void pipe_close_write(int idx)
{
    if (idx < 0 || idx >= MAX_PIPES) return;
    spinlock_acquire(&pipe_lock);
    pipe_t *p = &s_pipes[idx];
    if (p->in_use && p->writers > 0) {
        p->writers--;
        if (p->writers == 0 && p->readers == 0) p->in_use = false;
    }
    spinlock_release(&pipe_lock);
}

void pipe_close_read(int idx)
{
    if (idx < 0 || idx >= MAX_PIPES) return;
    spinlock_acquire(&pipe_lock);
    pipe_t *p = &s_pipes[idx];
    if (p->in_use && p->readers > 0) {
        p->readers--;
        if (p->readers == 0 && p->writers == 0) p->in_use = false;
    }
    spinlock_release(&pipe_lock);
}
