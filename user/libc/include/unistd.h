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
pid_t    spawn_as(const char *path, char **argv, uid_t uid, gid_t gid);
pid_t    waitpid(pid_t pid, int *status, int options);
long     listdir(const char *path, char *buf, long bufsz);
int      pipe(int pipefd[2]);

uid_t    getuid(void);
gid_t    getgid(void);
int      setuid(uid_t uid);
int      setgid(gid_t gid);
int      chown(const char *path, uid_t uid, gid_t gid);

int      creat(const char *path);
int      mkdir(const char *path, int mode);
int      unlink(const char *path);
int      chdir(const char *path);
char    *getcwd(char *buf, long size);

int      reboot(int cmd);
#define REBOOT_CMD_POWEROFF  0
#define REBOOT_CMD_RESTART   1

int      sleep_ms(long ms);
int      kill(int pid);
long     ps_list(char *buf, long bufsz);

/* O_CREAT flag */
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0x40
#define O_TRUNC   0x200
#define O_APPEND  0x400

#endif
