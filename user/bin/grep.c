#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define BUF_SZ 4096
#define LINE_SZ 512

static int grep_fd(const char *pattern, int fd, const char *filename, int show_name)
{
    static char buf[BUF_SZ];
    static char line[LINE_SZ];
    int line_len = 0;
    int matched = 0;
    long n;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (long i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n' || line_len == LINE_SZ - 1) {
                line[line_len] = '\0';
                if (strstr(line, pattern)) {
                    if (show_name) {
                        puts(filename);
                        write(1, ":", 1);
                    }
                    puts(line);
                    matched++;
                }
                line_len = 0;
            } else {
                line[line_len++] = c;
            }
        }
    }
    /* flush last line if no trailing newline */
    if (line_len > 0) {
        line[line_len] = '\0';
        if (strstr(line, pattern)) {
            if (show_name) { puts(filename); write(1, ":", 1); }
            puts(line);
            matched++;
        }
    }
    return matched;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        puts("usage: grep pattern [file...]");
        return 1;
    }
    const char *pattern = argv[1];
    int matched = 0;

    if (argc == 2) {
        matched = grep_fd(pattern, 0, "(stdin)", 0);
    } else {
        int show_name = (argc > 3);
        for (int i = 2; i < argc; i++) {
            int fd = open(argv[i], 0, 0);
            if (fd < 0) {
                printf("grep: %s: not found\n", argv[i]);
                continue;
            }
            matched += grep_fd(pattern, fd, argv[i], show_name);
            close(fd);
        }
    }
    return matched > 0 ? 0 : 1;
}
