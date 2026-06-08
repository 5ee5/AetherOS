#ifndef KERNEL_PROC_FD_H
#define KERNEL_PROC_FD_H

#include <stdbool.h>
#include <stdint.h>

#define MAX_FDS 32

typedef enum {
    FD_NONE = 0,
    FD_FILE,
    FD_SOCKET,
    FD_PIPE_READ,
    FD_PIPE_WRITE,
} fd_type_t;

typedef struct {
    bool      open;
    fd_type_t type;
    int       id;   /* vfs_fd / socket index / pipe index */
} fd_entry_t;

typedef struct {
    fd_entry_t fds[MAX_FDS];
} fd_table_t;

void      fd_table_init(fd_table_t *t);
int       fd_alloc(fd_table_t *t, int vfs_fd);
int       fd_alloc_socket(fd_table_t *t, int sock_idx);
int       fd_alloc_pipe(fd_table_t *t, int pipe_idx, fd_type_t type);
int       fd_set(fd_table_t *t, int slot, fd_type_t type, int id);
int       fd_to_vfs(fd_table_t *t, int fd);
fd_type_t fd_type(fd_table_t *t, int fd);
int       fd_id(fd_table_t *t, int fd);
void      fd_free(fd_table_t *t, int fd);

#endif
