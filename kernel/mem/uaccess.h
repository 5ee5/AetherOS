#ifndef KERNEL_MEM_UACCESS_H
#define KERNEL_MEM_UACCESS_H

#include <stdbool.h>
#include <stdint.h>

/* First byte above the user-accessible virtual range. */
#define USER_ADDR_LIMIT 0x800000000000ULL

/* True if [addr, addr+len) lies entirely within the user portion (no wrap). */
static inline bool user_range_ok(uint64_t addr, uint64_t len)
{
    return (addr + len >= addr) && (addr + len <= USER_ADDR_LIMIT);
}

/* Copy between user and kernel space with page-fault recovery: if the user
   side is unmapped/non-canonical the access is trapped via the exception
   table and the function returns false instead of panicking the kernel.
   Returns true only if all `len` bytes were transferred. */
bool copy_from_user(void *dst, const void *user_src, uint64_t len);
bool copy_to_user(void *user_dst, const void *src, uint64_t len);

/* Copy a NUL-terminated string from user space into dst (capacity maxlen).
   Returns the string length (excluding NUL) on success, or -1 on fault or if
   no terminator was found within maxlen. dst is always NUL-terminated on
   success. */
int64_t strncpy_from_user(char *dst, const void *user_src, uint64_t maxlen);

/* Return the fixup RIP registered for a faulting kernel RIP, or 0 if none.
   Used by the page-fault handler to recover from user-copy faults. */
uint64_t extable_lookup(uint64_t fault_rip);

#endif
