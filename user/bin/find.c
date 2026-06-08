#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define BUF 4096

static char s_pattern[256];
static int  s_has_pattern;
static char s_path[512];

static void do_find(const char *dir)
{
    static char buf[BUF];
    long n = listdir(dir, buf, sizeof(buf));
    if (n <= 0) return;

    const char *p = buf;
    while (p < buf + n) {
        int len = (int)strlen(p);
        if (len == 0) { p++; continue; }

        /* Build full path */
        int dlen = (int)strlen(dir);
        if (dlen + 1 + len + 1 > (int)sizeof(s_path)) { p += len + 1; continue; }
        memcpy(s_path, dir, (long)dlen);
        s_path[dlen] = '/';
        memcpy(s_path + dlen + 1, p, (long)(len + 1));

        /* Print if matches pattern (or no pattern) */
        if (!s_has_pattern || strstr(p, s_pattern))
            puts(s_path);

        /* Try to recurse (listdir on path returns >0 if it's a directory) */
        static char sub[BUF];
        long subn = listdir(s_path, sub, sizeof(sub));
        if (subn > 0) do_find(s_path);

        p += len + 1;
    }
}

int main(int argc, char **argv)
{
    const char *dir = ".";
    s_has_pattern = 0;

    if (argc >= 2) dir = argv[1];
    if (argc >= 3) {
        strncpy(s_pattern, argv[2], sizeof(s_pattern) - 1);
        s_has_pattern = 1;
    }

    do_find(dir);
    return 0;
}
