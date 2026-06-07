#include "proc/fd.h"

#include "lib/string.h"

void fd_table_init(fd_table_t *t)
{
    memset(t, 0, sizeof(*t));
}

int fd_alloc(fd_table_t *t, int vfs_fd)
{
    for (int i = 0; i < MAX_FDS; ++i) {
        if (!t->fds[i].open) {
            t->fds[i].open   = true;
            t->fds[i].vfs_fd = vfs_fd;
            return i;
        }
    }
    return -1;
}

int fd_to_vfs(fd_table_t *t, int fd)
{
    if (fd < 0 || fd >= MAX_FDS || !t->fds[fd].open) return -1;
    return t->fds[fd].vfs_fd;
}

void fd_free(fd_table_t *t, int fd)
{
    if (fd < 0 || fd >= MAX_FDS) return;
    t->fds[fd].open = false;
}
