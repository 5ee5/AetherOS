#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PASSWD_FILE "/etc/passwd"
#define BUF_SZ  2048
#define OUT_SZ  4096

static char s_in[BUF_SZ];
static char s_out[OUT_SZ];

static long read_line(char *buf, long maxlen)
{
    long n = read(0, buf, maxlen - 1);
    if (n < 0) n = 0;
    if (n > 0 && buf[n - 1] == '\n') n--;
    buf[n] = '\0';
    return n;
}

int main(int argc, char **argv)
{
    uid_t myuid = getuid();

    /* Determine which account to change. */
    char target[64];
    if (argc >= 2) {
        if (myuid != 0) { puts("passwd: only root can change another user's password"); return 1; }
        strncpy(target, argv[1], sizeof(target) - 1);
        target[sizeof(target) - 1] = '\0';
    } else {
        /* Default: change own account — look up name by uid. */
        int fd = open(PASSWD_FILE, 0, 0);
        if (fd < 0) { puts("passwd: cannot open /etc/passwd"); return 1; }
        long n = read(fd, s_in, BUF_SZ - 1); close(fd);
        if (n <= 0) { puts("passwd: cannot read /etc/passwd"); return 1; }
        s_in[n] = '\0';
        target[0] = '\0';
        char *p = s_in;
        while (*p) {
            char *name = p;
            char *c1 = strchr(p, ':');    if (!c1) break; *c1 = '\0';
            char *pass = c1 + 1;
            char *c2 = strchr(pass, ':'); if (!c2) break; *c2 = '\0';
            char *uid_s = c2 + 1;
            char *c3 = strchr(uid_s, ':'); if (!c3) break; *c3 = '\0';
            char *rest = c3 + 1;
            char *nl = strchr(rest, '\n'); if (nl) *nl = '\0';
            if ((uid_t)atoi(uid_s) == myuid) { strncpy(target, name, sizeof(target)-1); break; }
            p = nl ? nl + 1 : rest + strlen(rest);
        }
        if (!target[0]) { puts("passwd: current user not found in /etc/passwd"); return 1; }
    }

    /* Read /etc/passwd fresh for processing. */
    int fd = open(PASSWD_FILE, 0, 0);
    if (fd < 0) { puts("passwd: cannot open /etc/passwd"); return 1; }
    long n = read(fd, s_in, BUF_SZ - 1); close(fd);
    if (n <= 0) { puts("passwd: cannot read /etc/passwd"); return 1; }
    s_in[n] = '\0';

    /* If non-root, verify current password first. */
    if (myuid != 0) {
        char stored[64] = {0};
        char *p = s_in;
        while (*p) {
            char *name = p;
            char *c1 = strchr(p, ':');    if (!c1) break; *c1 = '\0';
            char *pass = c1 + 1;
            char *c2 = strchr(pass, ':'); if (!c2) break; *c2 = '\0';
            char *uid_s = c2 + 1;
            char *c3 = strchr(uid_s, ':'); if (!c3) break; *c3 = '\0';
            char *rest = c3 + 1;
            char *nl = strchr(rest, '\n'); if (nl) *nl = '\0';
            if (strcmp(name, target) == 0) { strncpy(stored, pass, 63); break; }
            p = nl ? nl + 1 : rest + strlen(rest);
        }
        /* Re-read since we mutated s_in above. */
        fd = open(PASSWD_FILE, 0, 0);
        n = read(fd, s_in, BUF_SZ - 1); close(fd);
        s_in[n] = '\0';

        printf("Current password: ");
        static char cur[64];
        read_line(cur, sizeof(cur));
        if (stored[0] != '\0' && strcmp(cur, stored) != 0) {
            puts("passwd: incorrect current password");
            return 1;
        }
    }

    /* Get new password (twice). */
    static char np1[64], np2[64];
    printf("New password: ");
    read_line(np1, sizeof(np1));
    printf("Retype new password: ");
    read_line(np2, sizeof(np2));
    if (strcmp(np1, np2) != 0) { puts("passwd: passwords do not match"); return 1; }

    /* Rebuild /etc/passwd with updated password for target. */
    long out_pos = 0;
    int found = 0;
    char *p = s_in;
    while (*p) {
        char *line_start = p;
        char *nl = strchr(p, '\n');
        char *line_end = nl ? nl : p + strlen(p);

        /* Tokenise this line (5 fields). */
        char tmp[256];
        long linelen = line_end - line_start;
        if (linelen >= (long)sizeof(tmp)) linelen = (long)sizeof(tmp) - 1;
        memcpy(tmp, line_start, (long)linelen);
        tmp[linelen] = '\0';

        char *f[5] = {0};
        char *tok = tmp;
        for (int i = 0; i < 5; i++) {
            f[i] = tok;
            char *sep = strchr(tok, ':');
            if (sep) { *sep = '\0'; tok = sep + 1; }
            else { break; }
        }

        if (f[0] && strcmp(f[0], target) == 0 && f[1] && f[2] && f[3] && f[4]) {
            /* Emit updated line. */
            long need = (long)strlen(f[0]) + 1 + (long)strlen(np1) + 1 +
                        (long)strlen(f[2]) + 1 + (long)strlen(f[3]) + 1 +
                        (long)strlen(f[4]) + 1;
            if (out_pos + need < OUT_SZ) {
                out_pos += (long)sprintf(s_out + out_pos, "%s:%s:%s:%s:%s\n",
                                         f[0], np1, f[2], f[3], f[4]);
            }
            found = 1;
        } else {
            /* Copy line verbatim. */
            long copy = linelen + (nl ? 1 : 0);
            if (out_pos + copy < OUT_SZ) {
                memcpy(s_out + out_pos, line_start, (long)copy);
                out_pos += copy;
            }
        }

        p = nl ? nl + 1 : line_end;
    }

    if (!found) { printf("passwd: user '%s' not found\n", target); return 1; }

    /* Write back with truncate. */
    fd = open(PASSWD_FILE, O_WRONLY | O_TRUNC, 0);
    if (fd < 0) { puts("passwd: cannot write /etc/passwd"); return 1; }
    write(fd, s_out, (long)out_pos);
    close(fd);

    printf("Password updated for %s.\n", target);
    return 0;
}
