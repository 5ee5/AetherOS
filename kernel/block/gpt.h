#ifndef KERNEL_BLOCK_GPT_H
#define KERNEL_BLOCK_GPT_H

#include <stdbool.h>
#include <stdint.h>

#include "block/blkdev.h"

typedef struct {
    uint64_t first_lba;
    uint64_t last_lba;
} gpt_partition_t;

/* Read the GPT from `dev` and fill `parts[0..max-1]`.
   Returns the number of valid partitions found (up to `max`). */
int gpt_read(blkdev_t *dev, gpt_partition_t *parts, int max);

#endif
