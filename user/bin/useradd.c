#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PASSWD_FILE "/etc/passwd"
#define BUF_SZ 2048

static char s_buf[BUF_SZ];

int main(int argc, char **argv)
{
    if (getuid() != 0) { puts("useradd: permission denied (must be root)"); return 1; }
    if (argc < 2) { puts("useradd: usage: useradd <username>"); return 1; }

    const char *username = argv[1];

    /* Basic username validation: non-empty, no colon or slash. */
    if (!username[0] || strchr(username, ':') || strchr(username, '/')) {
        puts("useradd: invalid username");
        return 1;
    }

    /* Read /etc/passwd to check for duplicate and find max uid. */
    int fd = open(PASSWD_FILE, 0, 0);
    if (fd < 0) { puts("useradd: cannot open /etc/passwd"); return 1; }
    long n = read(fd, s_buf, BUF_SZ - 1); close(fd);
    if (n < 0) n = 0;
    s_buf[n] = '\0';

    uid_t max_uid = 999;
    char *p = s_buf;
    while (*p) {
        char *name = p;
        char *c1 = strchr(p, ':');    if (!c1) break; *c1 = '\0';
        char *pass = c1 + 1;
        char *c2 = strchr(pass, ':'); if (!c2) break; *c2 = '\0';
        char *uid_s = c2 + 1;
        char *c3 = strchr(uid_s, ':'); if (!c3) break; *c3 = '\0';
        char *rest = c3 + 1;
        char *nl = strchr(rest, '\n'); if (nl) *nl = '\0';

        if (strcmp(name, username) == 0) {
            printf("useradd: user '%s' already exists\n", username);
            return 1;
        }

        uid_t u = (uid_t)atoi(uid_s);
        if (u >= 1000 && u > max_uid) max_uid = u;

        p = nl ? nl + 1 : rest + strlen(rest);
    }

    uid_t new_uid = max_uid + 1;
    if (new_uid < 1000) new_uid = 1000;

    /* Create home directory /home/<username>. */
    static char home[128];
    /* Ensure /home exists. */
    mkdir("/home", 0);
    snprintf(home, sizeof(home), "/home/%s", username);
    if (mkdir(home, 0) < 0) {
        printf("useradd: warning: could not create %s\n", home);
    }

    /* Append new entry to /etc/passwd with empty password. */
    fd = open(PASSWD_FILE, O_WRONLY | O_APPEND, 0);
    if (fd < 0) { puts("useradd: cannot write /etc/passwd"); return 1; }

    static char line[256];
    long llen = (long)snprintf(line, sizeof(line), "%s::%u:%u:%s\n",
                                username, new_uid, new_uid, home);
    write(fd, line, llen);
    close(fd);

    printf("User '%s' created (uid=%u). Use 'passwd %s' to set a password.\n",
           username, new_uid, username);
    return 0;
}
