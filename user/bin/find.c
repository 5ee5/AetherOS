#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define MAX_DEPTH  8
#define LIST_BUF   1024
#define PATH_MAX   512

static char s_pattern[256];
static int  s_has_pattern;

/* One listing buffer and one path buffer per recursion level.  The previous
   version declared both `static` *inside* the recursive function and wrote the
   assembled path into a single file-scope buffer, so descending into a
   subdirectory overwrote the parent's listing and path mid-iteration.  The user
   stack is only 16 KiB, so these stay out of it. */
static char s_lists[MAX_DEPTH][LIST_BUF];
static char s_paths[MAX_DEPTH][PATH_MAX];

static void do_find(const char *dir, int depth)
{
    if (depth >= MAX_DEPTH) return;

    char *buf = s_lists[depth];
    long n = listdir(dir, buf, LIST_BUF);
    if (n <= 0) return;

    char *path = s_paths[depth];
    int   dlen = (int)strlen(dir);
    /* Avoid emitting "//foo" when listing the root. */
    int   sep  = (dlen > 0 && dir[dlen - 1] == '/') ? 0 : 1;

    char *p   = buf;
    char *end = buf + n;
    while (p < end) {
        /* listdir returns NEWLINE-separated names (see ext2_list_dir).  This
           used to walk them with strlen()/+1 as if they were NUL-separated, so
           the very first "name" spanned the whole buffer and find printed one
           bogus path and stopped. */
        char *nl = p;
        while (nl < end && *nl != '\n') nl++;
        int len = (int)(nl - p);
        if (len == 0) { p = nl + 1; continue; }

        if (dlen + sep + len + 1 > PATH_MAX) { p = nl + 1; continue; }
        memcpy(path, dir, (size_t)dlen);
        if (sep) path[dlen] = '/';
        memcpy(path + dlen + sep, p, (size_t)len);
        path[dlen + sep + len] = '\0';

        if (!s_has_pattern || strstr(path + dlen + sep, s_pattern))
            puts(path);

        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
            do_find(path, depth + 1);

        p = nl + 1;
    }
}

int main(int argc, char **argv)
{
    const char *dir = ".";
    s_has_pattern = 0;

    if (argc >= 2) dir = argv[1];
    if (argc >= 3) {
        strncpy(s_pattern, argv[2], sizeof(s_pattern) - 1);
        s_pattern[sizeof(s_pattern) - 1] = '\0';
        s_has_pattern = 1;
    }

    do_find(dir, 0);
    return 0;
}
