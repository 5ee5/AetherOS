#include "proc/fd.h"

#include "lib/string.h"

void fd_table_init(fd_table_t *t)
{
    memset(t, 0, sizeof(*t));
}

/* Allocation always starts at 3: fds 0/1/2 belong to stdin/stdout/stderr, which
   are either console stubs (no table entry at all) or pipe ends placed there
   explicitly by fd_set.  Handing a socket or file out at fd 0 used to make
   read() serve it console input instead of the descriptor's real contents. */
static int fd_alloc_slot(fd_table_t *t, fd_type_t type, int id)
{
    for (int i = 3; i < MAX_FDS; ++i) {
        if (!t->fds[i].open) {
            t->fds[i].open = true;
            t->fds[i].type = type;
            t->fds[i].id   = id;
            return i;
        }
    }
    return -1;
}

int fd_alloc(fd_table_t *t, int vfs_fd)
{
    return fd_alloc_slot(t, FD_FILE, vfs_fd);
}

int fd_alloc_socket(fd_table_t *t, int sock_idx)
{
    return fd_alloc_slot(t, FD_SOCKET, sock_idx);
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

int fd_alloc_pipe(fd_table_t *t, int pipe_idx, fd_type_t type)
{
    return fd_alloc_slot(t, type, pipe_idx);
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
