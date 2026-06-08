#ifndef _UNISTD_H
#define _UNISTD_H

#include <sys/types.h>

ssize_t  read(int fd, void *buf, size_t n);
ssize_t  write(int fd, const void *buf, size_t n);
int      open(const char *path, int flags, int mode);
int      close(int fd);
void     _exit(int status);

pid_t    getpid(void);
pid_t    spawn(const char *path, char **argv);
pid_t    waitpid(pid_t pid, int *status, int options);
long     listdir(const char *path, char *buf, long bufsz);

#endif
