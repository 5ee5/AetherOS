#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define BUF      4096
#define LINE     512
#define MAXLINES 512

static char  s_lines[MAXLINES][LINE];
static int   s_count;

static void store(const char *s, int len)
{
    if (s_count >= MAXLINES) return;
    if (len >= LINE) len = LINE - 1;
    memcpy(s_lines[s_count], s, (long)len);
    s_lines[s_count][len] = '\0';
    s_count++;
}

static void read_lines(int fd)
{
    static char buf[BUF];
    static char line[LINE];
    int line_len = 0;
    long got;

    while ((got = read(fd, buf, sizeof(buf))) > 0) {
        for (long i = 0; i < got; i++) {
            char c = buf[i];
            if (c == '\n' || line_len == LINE - 1) {
                store(line, line_len);
                line_len = 0;
            } else {
                line[line_len++] = c;
            }
        }
    }
    if (line_len > 0) store(line, line_len);
}

static void insertion_sort(void)
{
    for (int i = 1; i < s_count; i++) {
        char tmp[LINE];
        memcpy(tmp, s_lines[i], LINE);
        int j = i - 1;
        while (j >= 0 && strcmp(s_lines[j], tmp) > 0) {
            memcpy(s_lines[j + 1], s_lines[j], LINE);
            j--;
        }
        memcpy(s_lines[j + 1], tmp, LINE);
    }
}

int main(int argc, char **argv)
{
    s_count = 0;
    if (argc < 2) {
        read_lines(0);
    } else {
        for (int i = 1; i < argc; i++) {
            int fd = open(argv[i], 0, 0);
            if (fd < 0) { printf("sort: %s: not found\n", argv[i]); continue; }
            read_lines(fd);
            close(fd);
        }
    }
    insertion_sort();
    for (int i = 0; i < s_count; i++) puts(s_lines[i]);
    return 0;
}
