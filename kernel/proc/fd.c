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
            t->fds[i].open = true;
            t->fds[i].type = FD_FILE;
            t->fds[i].id   = vfs_fd;
            return i;
        }
    }
    return -1;
}

int fd_alloc_socket(fd_table_t *t, int sock_idx)
{
    for (int i = 0; i < MAX_FDS; ++i) {
        if (!t->fds[i].open) {
            t->fds[i].open = true;
            t->fds[i].type = FD_SOCKET;
            t->fds[i].id   = sock_idx;
            return i;
        }
    }
    return -1;
}

int fd_to_vfs(fd_table_t *t, int fd)
{
    if (fd < 0 || fd >= MAX_FDS || !t->fds[fd].open) return -1;
    if (t->fds[fd].type != FD_FILE) return -1;
    return t->fds[fd].id;
}

fd_type_t fd_type(fd_table_t *t, int fd)
{
    if (fd < 0 || fd >= MAX_FDS || !t->fds[fd].open) return FD_NONE;
    return t->fds[fd].type;
}

int fd_id(fd_table_t *t, int fd)
{
    if (fd < 0 || fd >= MAX_FDS || !t->fds[fd].open) return -1;
    return t->fds[fd].id;
}

void fd_free(fd_table_t *t, int fd)
{
    if (fd < 0 || fd >= MAX_FDS) return;
    t->fds[fd].open = false;
    t->fds[fd].type = FD_NONE;
    t->fds[fd].id   = 0;
}
