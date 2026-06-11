#include "exec/elf.h"

#include <stdbool.h>
#include <stdint.h>

#include "core/serial.h"
#include "lib/string.h"
#include "mem/pmm.h"
#include "mem/vmm.h"
#include "os/elf64.h"

#define PAGE_SIZE 4096ULL
#define USER_STACK_PAGES 4ULL
#define USER_STACK_TOP   0x800000000000ULL   /* exclusive: first byte above stack */
#define USER_STACK_BASE  (USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE)

static uint64_t div_ceil(uint64_t n, uint64_t d)
{
    return (n + d - 1U) / d;
}

bool elf_load(const void *image, uint64_t size, elf_load_result_t *out)
{
    if (image == NULL || size < sizeof(struct elf64_ehdr)) {
        serial_write("elf: image too small\n");
        return false;
    }

    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)image;

    /* Validate ELF magic and class. */
    if (eh->e_ident[0] != ELFMAG0 || eh->e_ident[1] != ELFMAG1 ||
        eh->e_ident[2] != ELFMAG2 || eh->e_ident[3] != ELFMAG3) {
        serial_write("elf: bad magic\n");
        return false;
    }
    if (eh->e_ident[4] != ELFCLASS64) {
        serial_write("elf: not ELF64\n");
        return false;
    }
    if (eh->e_type != ET_EXEC) {
        serial_write("elf: not ET_EXEC\n");
        return false;
    }
    if (eh->e_machine != EM_X86_64) {
        serial_write("elf: not x86-64\n");
        return false;
    }
    if (eh->e_phoff == 0 || eh->e_phnum == 0) {
        serial_write("elf: no program headers\n");
        return false;
    }

    /* Create a fresh address space. */
    uint64_t cr3 = vmm_space_create();
    if (cr3 == 0) {
        serial_write("elf: vmm_space_create failed\n");
        return false;
    }

    /* Load each PT_LOAD segment. */
    for (uint16_t i = 0; i < eh->e_phnum; ++i) {
        uint64_t phdr_off = eh->e_phoff + (uint64_t)i * eh->e_phentsize;
        /* Overflow-safe bounds check: reject if the addition wrapped past
           2^64 or the header would extend past the loaded image. */
        if (phdr_off < eh->e_phoff || phdr_off > size ||
            size - phdr_off < sizeof(struct elf64_phdr)) {
            serial_write("elf: phdr out of bounds\n");
            return false;
        }
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)((const uint8_t *)image + phdr_off);

        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) {
            continue;
        }
        /* Overflow-safe bounds check on the segment's on-image file data. */
        if (ph->p_offset > size || size - ph->p_offset < ph->p_filesz) {
            serial_write("elf: segment file data out of bounds\n");
            return false;
        }

        uint64_t flags = VMM_USER;
        if (ph->p_flags & PF_W) {
            flags |= VMM_WRITABLE;
        }

        uint64_t vbase  = ph->p_vaddr & ~(PAGE_SIZE - 1ULL);
        uint64_t vend   = ph->p_vaddr + ph->p_memsz;
        /* Confine the segment to the user address range.  A crafted ELF could
           otherwise request a kernel-half p_vaddr; vmm_space_map walks the
           kernel page tables (shared across every address space), so such a
           mapping would corrupt global kernel memory and grant userspace
           access to it.  Also rejects the p_vaddr + p_memsz overflow case. */
        if (vend < ph->p_vaddr || ph->p_vaddr >= USER_STACK_TOP ||
            vend > USER_STACK_TOP) {
            serial_write("elf: segment vaddr out of user range\n");
            return false;
        }
        uint64_t npages = div_ceil(vend - vbase, PAGE_SIZE);

        const uint8_t *src = (const uint8_t *)image + ph->p_offset;
        uint64_t bytes_left = ph->p_filesz;

        for (uint64_t p = 0; p < npages; ++p) {
            uint64_t phys = pmm_alloc_page();
            if (phys == PMM_ALLOC_FAILED) {
                serial_write("elf: pmm_alloc_page failed\n");
                return false;
            }

            uint8_t *page_virt = (uint8_t *)(uintptr_t)
                (vmm_direct_map_base() + phys);
            memset(page_virt, 0, PAGE_SIZE);

            uint64_t virt_page = vbase + p * PAGE_SIZE;
            uint64_t page_off  = (p == 0) ? (ph->p_vaddr - vbase) : 0ULL;
            uint64_t copy_len  = (bytes_left > PAGE_SIZE - page_off)
                                     ? (PAGE_SIZE - page_off)
                                     : bytes_left;
            if (copy_len > 0) {
                memcpy(page_virt + page_off, src, copy_len);
                src        += copy_len;
                bytes_left -= copy_len;
            }

            if (!vmm_space_map(cr3, virt_page, phys, flags)) {
                serial_write("elf: vmm_space_map failed\n");
                return false;
            }
        }
    }

    /* Allocate and map user stack. */
    for (uint64_t p = 0; p < USER_STACK_PAGES; ++p) {
        uint64_t phys = pmm_alloc_page();
        if (phys == PMM_ALLOC_FAILED) {
            serial_write("elf: stack pmm_alloc failed\n");
            return false;
        }
        uint8_t *pv = (uint8_t *)(uintptr_t)(vmm_direct_map_base() + phys);
        memset(pv, 0, PAGE_SIZE);
        uint64_t virt = USER_STACK_BASE + p * PAGE_SIZE;
        if (!vmm_space_map(cr3, virt, phys, VMM_USER | VMM_WRITABLE)) {
            serial_write("elf: stack vmm_space_map failed\n");
            return false;
        }
    }

    out->entry   = eh->e_entry;
    out->user_sp = USER_STACK_TOP - 16ULL;  /* 16-byte aligned below top */
    out->cr3     = cr3;
    return true;
}
