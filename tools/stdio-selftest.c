/* Host-built check of tinylibc's string formatting contract.
 *
 * This exists because a silent mismatch here is invisible at the call site and
 * disarms code that looks correct.  snprintf used to return the *truncated*
 * length rather than the length it would have produced, which is what C99
 * requires and what callers use to detect truncation.  Every
 * `if (out + len >= size) return -1;` guard in user/libc/src/pwdb.c was therefore
 * unreachable, and `passwd` silently wrote a truncated /etc/shadow — dropping
 * other accounts' hashes — while reporting success.
 *
 * Linked against the real user/libc/src/stdio.c, so the contract is checked
 * rather than assumed.  Wired into the disk/usb image targets like mkpasswd's
 * selftest, so a regression fails the build instead of corrupting an image.
 */

#include <stdio.h>      /* tinylibc's, via -I (see the Makefile rule) */
#include <string.h>

/* tinylibc's stdio.c writes through these; route them to fd 1 so a failing run
   can still be read.  Declared exactly as unistd.h does. */
static long raw_write(const void *b, unsigned long n)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(1L), "D"(1L), "S"(b), "d"(n)
                     : "rcx", "r11", "memory");
    return r;
}
long write_all(int fd, const void *b, unsigned long n) { (void)fd; return raw_write(b, n); }
long write(int fd, const void *b, unsigned long n)     { (void)fd; return raw_write(b, n); }

static int failures;

static void check(const char *what, long got, long want)
{
    if (got != want) {
        printf("stdio-selftest: FAIL %s: got %ld want %ld\n", what, got, want);
        failures++;
    }
}

int main(void)
{
    char b[32];

    /* --- exact fit: cap holds the text plus its terminator --- */
    memset(b, '#', sizeof(b));
    check("exact return",      snprintf(b, 6, "%s", "abcde"), 5);
    check("exact length",      (long)strlen(b),               5);
    check("exact terminated",  b[5] == '\0',                  1);
    check("exact no overrun",  b[6] == '#',                   1);

    /* --- one byte too many: return the would-be length, write what fits --- */
    memset(b, '#', sizeof(b));
    check("truncated return",     snprintf(b, 6, "%s", "abcdef"), 6);
    check("truncated length",     (long)strlen(b),                5);
    check("truncated no overrun", b[6] == '#',                    1);

    /* --- badly overflowing: still exact, still terminated, still in bounds --- */
    memset(b, '#', sizeof(b));
    check("overflow return",     snprintf(b, 8, "%s", "0123456789ABCDEFGHIJ"), 20);
    check("overflow length",     (long)strlen(b),                              7);
    check("overflow no overrun", b[8] == '#',                                  1);

    /* --- cap 1: room for the terminator only --- */
    memset(b, '#', sizeof(b));
    check("cap1 return",     snprintf(b, 1, "%s", "xyz"), 3);
    check("cap1 length",     (long)strlen(b),             0);
    check("cap1 no overrun", b[1] == '#',                 1);

    /* --- cap 0: the buffer must not be touched at all --- */
    memset(b, '#', sizeof(b));
    check("cap0 return",    snprintf(b, 0, "%s", "xyz"), 3);
    check("cap0 untouched", b[0] == '#',                 1);

    /* --- the guard idiom pwdb.c relies on must actually fire --- */
    {
        const long DB_MAX = 16;
        long out = 0;
        int len = snprintf(b, (size_t)(DB_MAX - out), "%s", "0123456789ABCDEFGHIJ");
        check("pwdb overflow guard fires", (out + len >= DB_MAX), 1);
    }

    /* --- conversions used by the account tools and by stat --- */
    check("uid/gid line", snprintf(b, sizeof(b), "%s:x:%u:%u:%s", "bob", 1000U, 1000U, "/home/bob"),
          (long)strlen("bob:x:1000:1000:/home/bob"));
    check("uid/gid text", strcmp(b, "bob:x:1000:1000:/home/bob") == 0, 1);
    check("zero-padded octal", snprintf(b, sizeof(b), "(%04o)", 0644U), 6);
    check("zero-padded text",  strcmp(b, "(0644)") == 0, 1);

    if (failures == 0) {
        printf("stdio-selftest: all checks pass\n");
        return 0;
    }
    printf("stdio-selftest: %d failure(s)\n", failures);
    return 1;
}
