#ifndef KERNEL_EXEC_ELF_H
#define KERNEL_EXEC_ELF_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t entry;    /* user-mode entry point (RIP) */
    uint64_t user_sp;  /* initial user stack pointer (RSP) */
    uint64_t cr3;      /* PML4 physical address for the new address space */
} elf_load_result_t;

/* Load an ELF64 executable image into a fresh address space.
   Returns true on success and fills *out; false on any validation or OOM error. */
bool elf_load(const void *image, uint64_t size, elf_load_result_t *out);

#endif
