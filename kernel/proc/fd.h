#ifndef KERNEL_PROC_FD_H
#define KERNEL_PROC_FD_H

#include <stdbool.h>
#include <stdint.h>

#define MAX_FDS 32

typedef struct {
    bool    open;
    int     vfs_fd;    /* kernel vfs fd */
} fd_entry_t;

typedef struct {
    fd_entry_t fds[MAX_FDS];
} fd_table_t;

void    fd_table_init(fd_table_t *t);
int     fd_alloc(fd_table_t *t, int vfs_fd);
int     fd_to_vfs(fd_table_t *t, int fd);
void    fd_free(fd_table_t *t, int fd);

#endif
