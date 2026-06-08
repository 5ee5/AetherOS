#include "proc/pipe.h"
#include "lib/string.h"

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

int pipe_alloc(void)
{
    for (int i = 0; i < MAX_PIPES; i++) {
        if (!s_pipes[i].in_use) {
            memset(&s_pipes[i], 0, sizeof(s_pipes[i]));
            s_pipes[i].in_use = true;
            return i;
        }
    }
    return -1;
}

void pipe_free(int idx)
{
    if (idx < 0 || idx >= MAX_PIPES) return;
    s_pipes[idx].in_use = false;
}

/* Returns bytes written (may be less than len if buffer full). */
int pipe_write(int idx, const void *buf, uint32_t len)
{
    if (idx < 0 || idx >= MAX_PIPES || !s_pipes[idx].in_use) return -1;
    pipe_t *p = &s_pipes[idx];
    if (p->read_closed) return -1;
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t written = 0;
    while (written < len && p->count < PIPE_BUF) {
        uint32_t wp = (p->head + p->count) % PIPE_BUF;
        p->buf[wp] = src[written++];
        p->count++;
    }
    return (int)written;
}

/* Returns bytes read; 0 = EOF (write end closed and buffer empty);
   -2 = EAGAIN (no data yet, retry). */
int pipe_read(int idx, void *buf, uint32_t len)
{
    if (idx < 0 || idx >= MAX_PIPES || !s_pipes[idx].in_use) return -1;
    pipe_t *p = &s_pipes[idx];
    if (p->count == 0) {
        if (p->write_closed) return 0; /* EOF */
        return -2;                     /* no data yet */
    }
    uint8_t *dst = (uint8_t *)buf;
    uint32_t n = (len < p->count) ? len : p->count;
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = p->buf[p->head];
        p->head = (p->head + 1) % PIPE_BUF;
    }
    p->count -= n;
    return (int)n;
}

void pipe_close_write(int idx)
{
    if (idx < 0 || idx >= MAX_PIPES || !s_pipes[idx].in_use) return;
    s_pipes[idx].write_closed = true;
    if (s_pipes[idx].read_closed) pipe_free(idx);
}

void pipe_close_read(int idx)
{
    if (idx < 0 || idx >= MAX_PIPES || !s_pipes[idx].in_use) return;
    s_pipes[idx].read_closed = true;
    if (s_pipes[idx].write_closed) pipe_free(idx);
}
