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

/* Allocate a pipe fd; start from 3 to keep 0/1/2 as stdio stubs. */
int fd_alloc_pipe(fd_table_t *t, int pipe_idx, fd_type_t type)
{
    for (int i = 3; i < MAX_FDS; ++i) {
        if (!t->fds[i].open) {
            t->fds[i].open = true;
            t->fds[i].type = type;
            t->fds[i].id   = pipe_idx;
            return i;
        }
    }
    return -1;
}

/* Set a specific fd slot (used to install pipe ends in child processes). */
int fd_set(fd_table_t *t, int slot, fd_type_t type, int id)
{
    if (slot < 0 || slot >= MAX_FDS) return -1;
    t->fds[slot].open = true;
    t->fds[slot].type = type;
    t->fds[slot].id   = id;
    return slot;
}

void fd_free(fd_table_t *t, int fd)
{
    if (fd < 0 || fd >= MAX_FDS) return;
    t->fds[fd].open = false;
    t->fds[fd].type = FD_NONE;
    t->fds[fd].id   = 0;
}
