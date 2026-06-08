#ifndef KERNEL_BLOCK_BLKDEV_H
#define KERNEL_BLOCK_BLKDEV_H

#include <stdbool.h>
#include <stdint.h>

/* Simple synchronous block device abstraction. */
typedef struct blkdev {
    uint32_t sector_size;   /* bytes per sector (always 512 here) */
    uint64_t sector_count;
    /* Read `count` sectors starting at `lba` into `buf`.
       `buf` must point into the direct-map region (PMM-allocated). */
    bool (*read)(struct blkdev *self, uint64_t lba, uint32_t count, void *buf);
    /* Write `count` sectors from `buf` to `lba`.
       `buf` must point into the direct-map region (PMM-allocated). */
    bool (*write)(struct blkdev *self, uint64_t lba, uint32_t count, const void *buf);
    void *priv;
} blkdev_t;

static inline bool blkdev_read(blkdev_t *d, uint64_t lba, uint32_t count, void *buf)
{
    return d->read(d, lba, count, buf);
}

static inline bool blkdev_write(blkdev_t *d, uint64_t lba, uint32_t count, const void *buf)
{
    return d->write(d, lba, count, buf);
}

#endif
