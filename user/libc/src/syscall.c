#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>

static inline long __sc1(long nr, long a0)
{
    long r;
    __asm__ volatile("syscall"
        : "=a"(r) : "0"(nr), "D"(a0)
        : "rcx", "r11", "memory");
    return r;
}

static inline long __sc2(long nr, long a0, long a1)
{
    long r;
    __asm__ volatile("syscall"
        : "=a"(r) : "0"(nr), "D"(a0), "S"(a1)
        : "rcx", "r11", "memory");
    return r;
}

static inline long __sc3(long nr, long a0, long a1, long a2)
{
    long r;
    register long r10 __asm__("r10") = a2;
    __asm__ volatile("syscall"
        : "=a"(r) : "0"(nr), "D"(a0), "S"(a1), "d"(r10)
        : "rcx", "r11", "memory");
    return r;
}

ssize_t read(int fd, void *buf, size_t n)
{
    return (ssize_t)__sc3(0, (long)fd, (long)buf, (long)n);
}

ssize_t write(int fd, const void *buf, size_t n)
{
    return (ssize_t)__sc3(1, (long)fd, (long)buf, (long)n);
}

int open(const char *path, int flags, int mode)
{
    return (int)__sc3(2, (long)path, (long)flags, (long)mode);
}

int close(int fd)
{
    return (int)__sc1(3, (long)fd);
}

void _exit(int status)
{
    __sc1(60, (long)status);
    __builtin_unreachable();
}

void exit(int status)
{
    _exit(status);
}

pid_t getpid(void)
{
    return (pid_t)__sc1(39, 0);
}

pid_t waitpid(pid_t pid, int *status, int options)
{
    return (pid_t)__sc3(61, (long)pid, (long)status, (long)options);
}

pid_t spawn(const char *path, char **argv)
{
    return (pid_t)__sc2(500, (long)path, (long)argv);
}

long listdir(const char *path, char *buf, long bufsz)
{
    return __sc3(600, (long)path, (long)buf, bufsz);
}
