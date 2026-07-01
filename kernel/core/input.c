#include "core/input.h"

#include <stdint.h>

#include "sync/spinlock.h"

/* SPSC-ish ring shared by keyboard drivers (producers) and the tty read path
   (consumer). Guarded by an IRQ-safe lock since ps2kbd pushes from an ISR. */
#define INPUT_BUF 128

static volatile char    s_buf[INPUT_BUF];
static volatile uint32_t s_head;   /* next read  */
static volatile uint32_t s_tail;   /* next write */
static spinlock_t s_lock = SPINLOCK_INIT;

void input_push(char c)
{
    if (c == 0) return;
    uint64_t f = spinlock_acquire_irqsave(&s_lock);
    uint32_t next = (s_tail + 1) % INPUT_BUF;
    if (next != s_head) {          /* drop on overflow */
        s_buf[s_tail] = c;
        s_tail = next;
    }
    spinlock_release_irqrestore(&s_lock, f);
}

char input_getchar(void)
{
    char c = 0;
    uint64_t f = spinlock_acquire_irqsave(&s_lock);
    if (s_head != s_tail) {
        c = s_buf[s_head];
        s_head = (s_head + 1) % INPUT_BUF;
    }
    spinlock_release_irqrestore(&s_lock, f);
    return c;
}

bool input_ready(void)
{
    return s_head != s_tail;
}
