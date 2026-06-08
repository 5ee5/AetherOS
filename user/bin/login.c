#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#define PASSWD_FILE "/etc/passwd"
#define BUF_SZ 2048

static char s_buf[BUF_SZ];

/* Parse /etc/passwd (name:uid:gid:home) for username.
   Returns 0 on success, fills out_uid/out_gid/out_home. */
static int lookup_user(const char *username, uid_t *out_uid, gid_t *out_gid,
                       char *out_home)
{
    int fd = open(PASSWD_FILE, 0, 0);
    if (fd < 0) return -1;
    long n = read(fd, s_buf, BUF_SZ - 1);
    close(fd);
    if (n <= 0) return -1;
    s_buf[n] = '\0';

    char *p = s_buf;
    while (*p) {
        char *name = p;
        char *c1 = strchr(p, ':');
        if (!c1) break;
        *c1 = '\0';

        char *uid_s = c1 + 1;
        char *c2 = strchr(uid_s, ':');
        if (!c2) break;
        *c2 = '\0';

        char *gid_s = c2 + 1;
        char *c3 = strchr(gid_s, ':');
        if (!c3) break;
        *c3 = '\0';

        char *home = c3 + 1;
        char *nl = strchr(home, '\n');
        if (nl) *nl = '\0';

        if (strcmp(name, username) == 0) {
            *out_uid = (uid_t)atoi(uid_s);
            *out_gid = (gid_t)atoi(gid_s);
            strncpy(out_home, home, 255);
            out_home[255] = '\0';
            return 0;
        }

        p = nl ? nl + 1 : home + strlen(home);
    }
    return -1;
}

int main(void)
{
    static char username[64];
    static char home[256];

    for (;;) {
        printf("\nAetherOS login: ");

        long n = read(0, username, sizeof(username) - 1);
        if (n <= 0) continue;
        if (username[n - 1] == '\n') n--;
        username[n] = '\0';
        if (n == 0) continue;

        uid_t uid;
        gid_t gid;
        if (lookup_user(username, &uid, &gid, home) < 0) {
            puts("Login incorrect.");
            continue;
        }

        printf("Welcome to AetherOS, %s!\n", username);

        char *argv[] = { "shell", NULL };
        pid_t pid = spawn_as("/bin/shell", argv, uid, gid);
        if (pid < 0) {
            puts("login: could not start shell");
            continue;
        }
        waitpid(pid, NULL, 0);
    }
    return 0;
}
