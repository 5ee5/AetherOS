#ifndef KERNEL_FS_VFS_H
#define KERNEL_FS_VFS_H

#include <stdbool.h>
#include <stdint.h>

/* Mount the root filesystem (backed by the first AHCI disk + GPT partition 0 + ext2). */
bool vfs_init(void);

/* Open a file by absolute path.  Returns a kernel fd cookie (≥0) or <0 on error. */
int vfs_open(const char *path);

/* Read up to `size` bytes from an open fd into `buf`. Returns bytes read or <0. */
int64_t vfs_read(int fd, void *buf, uint32_t size);

/* Close a kernel fd. */
void vfs_close(int fd);

/* Return the file size for `path`, or UINT64_MAX on error. */
uint64_t vfs_file_size(const char *path);

/* List entries in directory `path` into `buf` as "name\n" lines.
   Returns bytes written (not including NUL), 0 on error. */
uint32_t vfs_listdir(const char *path, char *buf, uint32_t bufsz);

#endif
