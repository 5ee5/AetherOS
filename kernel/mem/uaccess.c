#include "mem/uaccess.h"

/* One exception-table entry: if a fault occurs at `fault` (a kernel RIP inside
   a user-copy routine), the handler resumes execution at `fixup`. Populated by
   the inline-asm copy routines below; collected into the .extable section by
   the linker script (__extable_start/__extable_end). */
struct extable_entry {
    uint64_t fault;
    uint64_t fixup;
};

extern const struct extable_entry __extable_start[];
extern const struct extable_entry __extable_end[];

uint64_t extable_lookup(uint64_t fault_rip)
{
    for (const struct extable_entry *e = __extable_start; e < __extable_end; ++e) {
        if (e->fault == fault_rip) {
            return e->fixup;
        }
    }
    return 0;
}

bool copy_from_user(void *dst, const void *user_src, uint64_t len)
{
    if (!user_range_ok((uint64_t)(uintptr_t)user_src, len)) {
        return false;
    }
    /* On fault during `rep movsb`, RCX holds the bytes not yet copied and the
       handler redirects RIP to label 2; len (RCX) is then non-zero. */
    __asm__ volatile(
        "1: rep movsb\n"
        "2:\n"
        ".pushsection .extable,\"a\"\n"
        ".quad 1b, 2b\n"
        ".popsection\n"
        : "+c"(len), "+S"(user_src), "+D"(dst)
        :
        : "memory");
    return len == 0;
}

bool copy_to_user(void *user_dst, const void *src, uint64_t len)
{
    if (!user_range_ok((uint64_t)(uintptr_t)user_dst, len)) {
        return false;
    }
    __asm__ volatile(
        "1: rep movsb\n"
        "2:\n"
        ".pushsection .extable,\"a\"\n"
        ".quad 1b, 2b\n"
        ".popsection\n"
        : "+c"(len), "+S"(src), "+D"(user_dst)
        :
        : "memory");
    return len == 0;
}

int64_t strncpy_from_user(char *dst, const void *user_src, uint64_t maxlen)
{
    const char *s = (const char *)user_src;
    for (uint64_t i = 0; i < maxlen; ++i) {
        char c;
        if (!copy_from_user(&c, s + i, 1)) {
            return -1;
        }
        dst[i] = c;
        if (c == '\0') {
            return (int64_t)i;
        }
    }
    return -1; /* not terminated within maxlen */
}
