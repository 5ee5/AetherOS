#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
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

static inline long __sc4(long nr, long a0, long a1, long a2, long a3)
{
    long r;
    register long _r10 __asm__("r10") = a3;
    __asm__ volatile("syscall"
        : "=a"(r) : "0"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(_r10)
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

uid_t getuid(void)  { return (uid_t)__sc1(102, 0); }
gid_t getgid(void)  { return (gid_t)__sc1(104, 0); }
int   setuid(uid_t uid) { return (int)__sc1(105, (long)uid); }
int   setgid(gid_t gid) { return (int)__sc1(106, (long)gid); }
int   chown(const char *path, uid_t uid, gid_t gid) { return (int)__sc3(92, (long)path, (long)uid, (long)gid); }

pid_t spawn(const char *path, char **argv)
{
    /* Pass -1 for stdin/stdout override args so no fd inheritance occurs. */
    return (pid_t)__sc4(500, (long)path, (long)argv, -1L, -1L);
}

pid_t spawn_as(const char *path, char **argv, uid_t uid, gid_t gid)
{
    return (pid_t)__sc4(501, (long)path, (long)argv, (long)uid, (long)gid);
}

int pipe(int pipefd[2])
{
    return (int)__sc1(22, (long)pipefd);
}

long listdir(const char *path, char *buf, long bufsz)
{
    return __sc3(600, (long)path, (long)buf, bufsz);
}

long listdir_all(const char *path, char *buf, long bufsz)
{
    return __sc4(600, (long)path, (long)buf, bufsz, 1L);
}

int stat(const char *path, struct stat *buf)
{
    return (int)__sc2(4, (long)path, (long)buf);
}

int creat(const char *path)
{
    return (int)__sc1(601, (long)path);
}

int mkdir(const char *path, int mode)
{
    (void)mode;
    return (int)__sc1(83, (long)path);
}

int unlink(const char *path)
{
    return (int)__sc1(87, (long)path);
}

int rename(const char *old_path, const char *new_path)
{
    return (int)__sc2(82, (long)old_path, (long)new_path);
}

int chdir(const char *path)
{
    return (int)__sc1(80, (long)path);
}

char *getcwd(char *buf, long size)
{
    long r = __sc2(79, (long)buf, size);
    return (r >= 0) ? buf : (char *)0;
}

int socket(int domain, int type, int proto)
{
    return (int)__sc3(41, (long)domain, (long)type, (long)proto);
}

int connect(int fd, const struct sockaddr_in *addr, int addrlen)
{
    return (int)__sc3(42, (long)fd, (long)addr, (long)addrlen);
}

long send(int fd, const void *buf, long len, int flags)
{
    return __sc4(44, (long)fd, (long)buf, len, (long)flags);
}

long recv(int fd, void *buf, long len, int flags)
{
    return __sc4(45, (long)fd, (long)buf, len, (long)flags);
}

int dns_resolve(const char *hostname, uint32_t *ip_out)
{
    return (int)__sc2(602, (long)hostname, (long)ip_out);
}

int chmod(const char *path, int mode)
{
    return (int)__sc2(90, (long)path, (long)mode);
}

int reboot(int cmd)
{
    return (int)__sc1(169, (long)cmd);
}

int sleep_ms(long ms)
{
    return (int)__sc1(35, ms);
}

int kill(int pid)
{
    return (int)__sc1(62, (long)pid);
}

long ps_list(char *buf, long bufsz)
{
    return __sc2(603, (long)buf, bufsz);
}
