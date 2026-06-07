#ifndef KERNEL_MEM_PMM_H
#define KERNEL_MEM_PMM_H

#include <stdbool.h>
#include <stdint.h>

#include "os/bootinfo.h"

#define PMM_ALLOC_FAILED UINT64_MAX

void pmm_init(const struct os_boot_info *boot_info);
uint64_t pmm_alloc_page(void);
void pmm_free_page(uint64_t physical_address);
uint64_t pmm_total_pages(void);
uint64_t pmm_free_pages(void);
bool pmm_address_in_range(uint64_t physical_address);

#endif

