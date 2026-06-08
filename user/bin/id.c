#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUF_SZ 1024

static const char *name_for_id(uid_t id, char *buf, long bufsz)
{
    int fd = open("/etc/passwd", 0, 0);
    if (fd < 0) return NULL;
    long n = read(fd, buf, bufsz - 1);
    close(fd);
    if (n <= 0) return NULL;
    buf[n] = '\0';

    char *p = buf;
    while (*p) {
        char *name = p;
        char *c1 = strchr(p, ':');
        if (!c1) break;
        *c1 = '\0';

        char *id_s = c1 + 1;
        char *c2 = strchr(id_s, ':');
        if (!c2) break;
        *c2 = '\0';

        char *rest = c2 + 1;
        char *nl = strchr(rest, '\n');
        if (nl) *nl = '\0';

        if ((uid_t)atoi(id_s) == id) return name;

        p = nl ? nl + 1 : rest + strlen(rest);
    }
    return NULL;
}

int main(void)
{
    uid_t uid = getuid();
    gid_t gid = getgid();

    static char ubuf[BUF_SZ];
    static char gbuf[BUF_SZ];

    const char *uname = name_for_id(uid, ubuf, BUF_SZ);
    const char *gname = name_for_id(gid, gbuf, BUF_SZ);

    if (uname)
        printf("uid=%u(%s) ", uid, uname);
    else
        printf("uid=%u ", uid);

    if (gname)
        printf("gid=%u(%s)\n", gid, gname);
    else
        printf("gid=%u\n", gid);

    return 0;
}
