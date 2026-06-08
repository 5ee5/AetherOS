#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PASSWD_FILE "/etc/passwd"
#define BUF_SZ 2048

static char s_buf[BUF_SZ];

/* Look up stored password for a given uid. Returns 0 on success. */
static int passwd_for_uid(uid_t uid, char *out_pass, char *out_name)
{
    int fd = open(PASSWD_FILE, 0, 0);
    if (fd < 0) return -1;
    long n = read(fd, s_buf, BUF_SZ - 1); close(fd);
    if (n <= 0) return -1;
    s_buf[n] = '\0';

    /* Format: name:password:uid:gid:home */
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

        if ((uid_t)atoi(uid_s) == uid) {
            strncpy(out_pass, pass, 63); out_pass[63] = '\0';
            if (out_name) { strncpy(out_name, name, 63); out_name[63] = '\0'; }
            return 0;
        }
        p = nl ? nl + 1 : rest + strlen(rest);
    }
    return -1;
}

int main(int argc, char **argv)
{
    if (argc < 2) { puts("usage: sudo <command> [args...]"); return 1; }

    /* getuid() returns the real (calling) uid; geteuid() would return 0
       because /bin/sudo has the setuid bit set. */
    uid_t ruid = getuid();

    static char stored_pass[64];
    static char username[64];

    if (passwd_for_uid(ruid, stored_pass, username) < 0) {
        puts("sudo: user not found in /etc/passwd");
        return 1;
    }

    /* Prompt for password unless the account has no password set. */
    if (stored_pass[0] != '\0') {
        printf("[sudo] password for %s: ", username);
        static char entered[64];
        long n = read(0, entered, sizeof(entered) - 1);
        if (n < 0) n = 0;
        if (n > 0 && entered[n - 1] == '\n') n--;
        entered[n] = '\0';

        if (strcmp(entered, stored_pass) != 0) {
            puts("sudo: authentication failure");
            return 1;
        }
    }

    /* euid=0 (setuid binary), so spawn_as is allowed. */
    pid_t pid = spawn_as(argv[1], &argv[1], 0, 0);
    if (pid < 0) {
        printf("sudo: cannot execute %s\n", argv[1]);
        return 1;
    }
    waitpid(pid, NULL, 0);
    return 0;
}
