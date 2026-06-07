#ifndef KERNEL_MEM_VMM_H
#define KERNEL_MEM_VMM_H

#include <stdbool.h>
#include <stdint.h>

#include "os/bootinfo.h"

/* Flags for vmm_map / vmm_protect (raw PTE bits, PAGE_PRESENT always set). */
#define VMM_WRITABLE 0x002ULL
#define VMM_USER     0x004ULL

void vmm_init(const struct os_boot_info *boot_info);
bool vmm_virt_to_phys(uint64_t virtual_address, uint64_t *physical_address);
uint64_t vmm_direct_map_base(void);
uint64_t vmm_pml4_phys(void);
bool vmm_map(uint64_t virt, uint64_t phys, uint64_t flags);
bool vmm_unmap(uint64_t virt);
bool vmm_protect(uint64_t virt, uint64_t flags);

/* Per-process address space API. */
uint64_t vmm_space_create(void);
void     vmm_space_destroy(uint64_t pml4_phys);
bool     vmm_space_map(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags);
void     vmm_space_switch(uint64_t pml4_phys);

#endif

