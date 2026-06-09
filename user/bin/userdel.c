#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PASSWD_FILE "/etc/passwd"
#define BUF_SZ 2048

static char s_in[BUF_SZ];
static char s_out[BUF_SZ];

int main(int argc, char **argv)
{
    if (getuid() != 0) { puts("userdel: permission denied (must be root)"); return 1; }
    if (argc < 2) { puts("userdel: usage: userdel <username>"); return 1; }

    const char *username = argv[1];

    if (strcmp(username, "root") == 0) {
        puts("userdel: cannot delete root");
        return 1;
    }

    int fd = open(PASSWD_FILE, 0, 0);
    if (fd < 0) { puts("userdel: cannot open /etc/passwd"); return 1; }
    long n = read(fd, s_in, BUF_SZ - 1);
    close(fd);
    if (n < 0) n = 0;
    s_in[n] = '\0';

    /* Copy every line except the matching user into s_out. */
    int found = 0;
    long out_len = 0;
    char *p = s_in;
    while (*p) {
        char *line_start = p;
        char *nl = strchr(p, '\n');
        long line_len = nl ? (nl - p + 1) : (long)strlen(p);

        /* Extract name (up to first colon). */
        char *colon = strchr(p, ':');
        int match = 0;
        if (colon) {
            long name_len = colon - p;
            if ((long)strlen(username) == name_len &&
                strncmp(username, p, (long)name_len) == 0) {
                match = 1;
                found = 1;
            }
        }

        if (!match) {
            if (out_len + line_len < BUF_SZ) {
                memcpy(s_out + out_len, line_start, line_len);
                out_len += line_len;
            }
        }

        p = nl ? nl + 1 : p + strlen(p);
    }

    if (!found) {
        printf("userdel: user '%s' not found\n", username);
        return 1;
    }

    fd = open(PASSWD_FILE, O_WRONLY | O_TRUNC, 0);
    if (fd < 0) { puts("userdel: cannot write /etc/passwd"); return 1; }
    write(fd, s_out, out_len);
    close(fd);

    printf("User '%s' deleted.\n", username);
    return 0;
}
