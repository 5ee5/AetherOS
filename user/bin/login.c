#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#define PASSWD_FILE "/etc/passwd"
#define BUF_SZ 2048

static char s_buf[BUF_SZ];

/* Parse /etc/passwd (name:password:uid:gid:home) for username.
   Returns 0 on success, fills out fields. */
static int lookup_user(const char *username, char *out_pass,
                       uid_t *out_uid, gid_t *out_gid, char *out_home)
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
        char *c1 = strchr(p, ':');   if (!c1) break; *c1 = '\0';
        char *pass = c1 + 1;
        char *c2 = strchr(pass, ':'); if (!c2) break; *c2 = '\0';
        char *uid_s = c2 + 1;
        char *c3 = strchr(uid_s, ':'); if (!c3) break; *c3 = '\0';
        char *gid_s = c3 + 1;
        char *c4 = strchr(gid_s, ':'); if (!c4) break; *c4 = '\0';
        char *home = c4 + 1;
        char *nl = strchr(home, '\n'); if (nl) *nl = '\0';

        if (strcmp(name, username) == 0) {
            strncpy(out_pass, pass, 63); out_pass[63] = '\0';
            *out_uid = (uid_t)atoi(uid_s);
            *out_gid = (gid_t)atoi(gid_s);
            strncpy(out_home, home, 255); out_home[255] = '\0';
            return 0;
        }

        p = nl ? nl + 1 : home + strlen(home);
    }
    return -1;
}

int main(void)
{
    static char username[64];
    static char password[64];
    static char stored_pass[64];
    static char home[256];

    for (;;) {
        printf("\nAetherOS login: ");

        long n = read(0, username, sizeof(username) - 1);
        if (n <= 0) continue;
        if (username[n - 1] == '\n') n--;
        username[n] = '\0';
        if (n == 0) continue;

        printf("Password: ");
        long pn = read(0, password, sizeof(password) - 1);
        if (pn < 0) pn = 0;
        if (pn > 0 && password[pn - 1] == '\n') pn--;
        password[pn] = '\0';

        uid_t uid;
        gid_t gid;
        if (lookup_user(username, stored_pass, &uid, &gid, home) < 0) {
            puts("Login incorrect.");
            continue;
        }

        /* Empty stored password means no password required. */
        if (stored_pass[0] != '\0' && strcmp(password, stored_pass) != 0) {
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
