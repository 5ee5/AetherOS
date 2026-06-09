#ifndef KERNEL_FS_VFS_H
#define KERNEL_FS_VFS_H

#include <stdbool.h>
#include <stdint.h>

#include "fs/ext2.h"

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
   flags bit 0: include "." and "..".
   Returns bytes written (not including NUL), 0 on error. */
uint32_t vfs_listdir(const char *path, char *buf, uint32_t bufsz, uint32_t flags);

/* Write up to `size` bytes from `buf` to open fd.  Returns bytes written or <0. */
int64_t vfs_write(int fd, const void *buf, uint32_t size);

/* Open (or create if O_CREAT) a file.  flags: 0=read, 1=write, 0x40=O_CREAT. */
int vfs_open_ex(const char *path, int flags);

/* Create a regular file at `path`.  Returns 0 on success or -1 on error. */
int vfs_creat(const char *path);

/* Create a directory at `path`.  Returns 0 on success or -1 on error. */
int vfs_mkdir(const char *path);

/* Remove the file at `path`.  Returns 0 on success or -1 on error. */
int vfs_unlink(const char *path);

/* Change working directory for the current process. */
int vfs_chdir(const char *path);

/* Get working directory for the current process into `buf`. */
int vfs_getcwd(char *buf, uint32_t size);

/* Return the mode, owner uid, and owner gid of a file. Returns 0 on success, -1 on error. */
int vfs_file_stat(const char *path, uint16_t *out_mode, uint32_t *out_uid, uint32_t *out_gid);

/* Set the owner uid/gid of a file. Returns 0 on success, -1 on error. */
int vfs_chown(const char *path, uint32_t uid, uint32_t gid);

/* Change permission bits of a file. Returns 0 on success, -1 on error. */
int vfs_chmod(const char *path, uint16_t mode);

/* Fill `out` with full file metadata. Returns 0 on success, -1 on error. */
int vfs_stat(const char *path, ext2_stat_t *out);

/* Rename or move `old_path` to `new_path`. Returns 0 on success, -1 on error. */
int vfs_rename(const char *old_path, const char *new_path);

#endif
