#ifndef KERNEL_DRIVERS_NVME_H
#define KERNEL_DRIVERS_NVME_H

#include <stdbool.h>
#include <stdint.h>

bool nvme_init(void);
int  nvme_first_ns(void);   /* 1 if namespace available, -1 if not */
bool nvme_read_sectors(uint64_t lba, uint16_t count, void *buf);
bool nvme_write_sectors(uint64_t lba, uint16_t count, const void *buf);

#endif
