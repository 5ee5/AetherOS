#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUF   4096
#define LINE  512
#define MAXLINES 1024

static char  s_lines[MAXLINES][LINE];
static int   s_head;   /* oldest line index */
static int   s_count;  /* lines stored      */

static void store_line(const char *s, int len)
{
    if (len >= LINE) len = LINE - 1;
    int idx = (s_head + s_count) % MAXLINES;
    memcpy(s_lines[idx], s, (long)len);
    s_lines[idx][len] = '\0';
    if (s_count < MAXLINES) {
        s_count++;
    } else {
        s_head = (s_head + 1) % MAXLINES;
    }
}

static void tail_fd(int fd, int n)
{
    static char buf[BUF];
    static char line[LINE];
    int line_len = 0;
    long got;

    s_head = 0; s_count = 0;

    while ((got = read(fd, buf, sizeof(buf))) > 0) {
        for (long i = 0; i < got; i++) {
            char c = buf[i];
            if (c == '\n' || line_len == LINE - 1) {
                store_line(line, line_len);
                line_len = 0;
            } else {
                line[line_len++] = c;
            }
        }
    }
    if (line_len > 0) store_line(line, line_len);

    int start = s_count > n ? s_count - n : 0;
    for (int i = start; i < s_count; i++)
        puts(s_lines[(s_head + i) % MAXLINES]);
}

int main(int argc, char **argv)
{
    int n = 10;
    int first_file = 1;

    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == 'n') {
        n = (int)strtol(argv[1] + 2, 0, 10);
        if (n <= 0) n = 10;
        first_file = 2;
    }

    if (first_file >= argc) {
        tail_fd(0, n);
    } else {
        for (int i = first_file; i < argc; i++) {
            int fd = open(argv[i], 0, 0);
            if (fd < 0) { printf("tail: %s: not found\n", argv[i]); continue; }
            if (argc - first_file > 1) printf("==> %s <==\n", argv[i]);
            tail_fd(fd, n);
            close(fd);
        }
    }
    return 0;
}
