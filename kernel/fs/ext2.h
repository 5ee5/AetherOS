#ifndef KERNEL_FS_EXT2_H
#define KERNEL_FS_EXT2_H

#include <stdbool.h>
#include <stdint.h>

#include "block/blkdev.h"

typedef struct ext2_fs ext2_fs_t;

/* Mount an ext2 filesystem from `dev`.
   `part_offset_lba` is the partition start (sectors from device LBA 0).
   Returns non-NULL on success; caller must not free. */
ext2_fs_t *ext2_mount(blkdev_t *dev, uint64_t part_offset_lba);

/* Look up `path` (absolute, e.g. "/hello.txt") in the filesystem.
   On success fills `inode_out` with the inode number and returns true. */
bool ext2_lookup(ext2_fs_t *fs, const char *path, uint32_t *inode_out);

/* Read up to `size` bytes from inode `ino` starting at byte offset `off`.
   `buf` must point into the direct-map region (PMM-allocated).
   Returns bytes read (may be less than requested at EOF). */
int64_t ext2_read(ext2_fs_t *fs, uint32_t ino, uint64_t off, void *buf, uint32_t size);

/* Return the file size of inode `ino`. */
uint64_t ext2_file_size(ext2_fs_t *fs, uint32_t ino);

/* List directory entries of `dir_ino` into `buf` as "name\n" lines.
   Skips "." and "..".  Returns total bytes written (not including NUL). */
uint32_t ext2_list_dir(ext2_fs_t *fs, uint32_t dir_ino, char *buf, uint32_t bufsz);

/* Look up a path and return its inode number, or 0 on error.
   (Wraps ext2_lookup for callers that need the inode.) */
uint32_t ext2_lookup_ino(ext2_fs_t *fs, const char *path);

/* Write up to `size` bytes to inode `ino` starting at byte offset `off`.
   Allocates data blocks as needed.  Returns bytes written or <0 on error. */
int64_t ext2_write(ext2_fs_t *fs, uint32_t ino, uint64_t off,
                   const void *buf, uint32_t size);

/* Create a regular file at `path`.  Returns inode number or 0 on error. */
uint32_t ext2_create(ext2_fs_t *fs, const char *path);

/* Create a directory at `path`.  Returns inode number or 0 on error. */
uint32_t ext2_mkdir(ext2_fs_t *fs, const char *path);

/* Remove the file at `path` (decrements link count; frees on last link).
   Returns true on success. */
bool ext2_unlink(ext2_fs_t *fs, const char *path);

/* Truncate inode `ino` to zero length (frees all data blocks). */
bool ext2_truncate(ext2_fs_t *fs, uint32_t ino);

/* Read i_mode and i_uid from inode `ino`. Returns true on success. */
bool ext2_inode_stat(ext2_fs_t *fs, uint32_t ino, uint16_t *out_mode, uint32_t *out_uid);

/* Set i_uid and i_gid on inode `ino`. Returns true on success. */
bool ext2_inode_chown(ext2_fs_t *fs, uint32_t ino, uint32_t uid, uint32_t gid);

#endif
