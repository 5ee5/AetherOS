#include "test/ktest.h"
#include "test/tests.h"

#include <stdint.h>

#include "proc/pipe.h"

/* Guards the reference counting that makes shell pipelines work.  The shell
   creates a pipe, hands an end to each stage, then closes its own copies while
   the children are still running — so a close must drop one reference, not tear
   the end down.  With the old boolean flags the parent's close ended the pipe
   immediately and `ls | wc` transferred nothing. */
void test_pipe_run(void)
{
    ktest_suite("pipe");

    char buf[16];

    int idx = pipe_alloc();
    KTEST_ASSERT(idx >= 0);
    if (idx < 0) return;

    /* A fresh pipe has no data but a live writer: not EOF, just nothing yet. */
    KTEST_ASSERT(pipe_read(idx, buf, sizeof(buf)) == -2);

    /* A child inherits the write end, then the parent closes its own copy. */
    pipe_dup_write(idx);
    pipe_close_write(idx);

    /* The child's write end is still open, so this must NOT report EOF. */
    KTEST_ASSERT(pipe_read(idx, buf, sizeof(buf)) == -2);

    /* ...and writing from the surviving end must still succeed. */
    KTEST_ASSERT(pipe_write(idx, "hi", 2) == 2);
    KTEST_ASSERT(pipe_read(idx, buf, 2) == 2);
    KTEST_ASSERT(buf[0] == 'h' && buf[1] == 'i');

    /* Buffered data must drain before EOF is reported, even with no writers. */
    KTEST_ASSERT(pipe_write(idx, "xy", 2) == 2);
    pipe_close_write(idx);                 /* last writer gone */
    KTEST_ASSERT(pipe_read(idx, buf, 2) == 2);
    KTEST_ASSERT(pipe_read(idx, buf, sizeof(buf)) == 0);   /* now EOF */

    /* With every reader gone a write reports EPIPE rather than buffering. */
    pipe_close_read(idx);
    KTEST_ASSERT(pipe_write(idx, "z", 1) < 0);

    /* Both ends released, so the slot must be reusable. */
    int again = pipe_alloc();
    KTEST_ASSERT(again >= 0);
    if (again >= 0) { pipe_close_read(again); pipe_close_write(again); }
}
