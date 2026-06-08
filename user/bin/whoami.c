#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUF_SZ 2048

int main(void)
{
    uid_t uid = getuid();

    int fd = open("/etc/passwd", 0, 0);
    if (fd < 0) { printf("%u\n", uid); return 0; }

    static char buf[BUF_SZ];
    long n = read(fd, buf, BUF_SZ - 1);
    close(fd);
    if (n <= 0) { printf("%u\n", uid); return 0; }
    buf[n] = '\0';

    /* Format: name:password:uid:gid:home */
    char *p = buf;
    while (*p) {
        char *name = p;
        char *c1 = strchr(p, ':');    if (!c1) break; *c1 = '\0';
        char *pass = c1 + 1;
        char *c2 = strchr(pass, ':'); if (!c2) break; *c2 = '\0';
        char *uid_s = c2 + 1;
        char *c3 = strchr(uid_s, ':'); if (!c3) break; *c3 = '\0';
        char *rest = c3 + 1;
        char *nl = strchr(rest, '\n'); if (nl) *nl = '\0';

        if ((uid_t)atoi(uid_s) == uid) { puts(name); return 0; }

        p = nl ? nl + 1 : rest + strlen(rest);
    }

    printf("%u\n", uid);
    return 0;
}
