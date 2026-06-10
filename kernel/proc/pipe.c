#include "proc/pipe.h"
#include "lib/string.h"
#include "sync/spinlock.h"

#define PIPE_BUF  4096
#define MAX_PIPES 8

typedef struct {
    bool     in_use;
    uint8_t  buf[PIPE_BUF];
    uint32_t head;          /* next read position  */
    uint32_t count;         /* bytes buffered      */
    bool     write_closed;
    bool     read_closed;
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
            spinlock_release(&pipe_lock);
            return i;
        }
    }
    spinlock_release(&pipe_lock);
    return -1;
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
    if (!p->in_use || p->read_closed) { spinlock_release(&pipe_lock); return -1; }
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
        int rv = p->write_closed ? 0 /* EOF */ : -2 /* no data yet */;
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

void pipe_close_write(int idx)
{
    if (idx < 0 || idx >= MAX_PIPES) return;
    spinlock_acquire(&pipe_lock);
    if (s_pipes[idx].in_use) {
        s_pipes[idx].write_closed = true;
        if (s_pipes[idx].read_closed) s_pipes[idx].in_use = false; /* inlined free */
    }
    spinlock_release(&pipe_lock);
}

void pipe_close_read(int idx)
{
    if (idx < 0 || idx >= MAX_PIPES) return;
    spinlock_acquire(&pipe_lock);
    if (s_pipes[idx].in_use) {
        s_pipes[idx].read_closed = true;
        if (s_pipes[idx].write_closed) s_pipes[idx].in_use = false; /* inlined free */
    }
    spinlock_release(&pipe_lock);
}
