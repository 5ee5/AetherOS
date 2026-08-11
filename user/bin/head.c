#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUF  4096
#define LINE 512

static void head_fd(int fd, int n)
{
    static char buf[BUF];
    static char line[LINE];
    int line_len = 0, printed = 0;
    long got;

    while (printed < n && (got = read(fd, buf, sizeof(buf))) > 0) {
        for (long i = 0; i < got && printed < n; i++) {
            char c = buf[i];
            if (c == '\n' || line_len == LINE - 1) {
                line[line_len] = '\0';
                puts(line);
                line_len = 0;
                printed++;
            } else {
                line[line_len++] = c;
            }
        }
    }
    if (line_len > 0 && printed < n) {
        line[line_len] = '\0';
        puts(line);
    }
}

int main(int argc, char **argv)
{
    int n = 10;
    int first_file = 1;

    /* Accept -N, -nN and -n N.  Only -nN worked before, so `head -2` and
       `head -n 2` were both taken as filenames ("head: -2: not found"). */
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] != '\0') {
        const char *spec = argv[1] + 1;
        if (*spec == 'n') {
            spec++;
            if (*spec == '\0' && argc > 2) {      /* "-n N" */
                spec = argv[2];
                first_file = 3;
            } else {                               /* "-nN" */
                first_file = 2;
            }
        } else {                                   /* "-N" */
            first_file = 2;
        }
        n = (int)strtol(spec, 0, 10);
        if (n <= 0) n = 10;
    }

    if (first_file >= argc) {
        head_fd(0, n);
    } else {
        for (int i = first_file; i < argc; i++) {
            int fd = open(argv[i], 0, 0);
            if (fd < 0) { printf("head: %s: not found\n", argv[i]); continue; }
            if (argc - first_file > 1) printf("==> %s <==\n", argv[i]);
            head_fd(fd, n);
            close(fd);
        }
    }
    return 0;
}
