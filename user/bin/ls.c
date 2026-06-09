#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static char ls_buf[8192];
static char cwd_buf[256];

static void print_rjust(unsigned val, int width)
{
    char tmp[12];
    int i = 0;
    if (val == 0) { tmp[i++] = '0'; }
    else { while (val) { tmp[i++] = (char)('0' + val % 10); val /= 10; } }
    for (int j = i; j < width; j++) putchar(' ');
    while (i--) putchar(tmp[i]);
}

static void fmt_mode(uint16_t mode, char *out)
{
    out[0] = S_ISDIR(mode) ? 'd' : '-';
    out[1] = (mode & 0400u) ? 'r' : '-';
    out[2] = (mode & 0200u) ? 'w' : '-';
    out[3] = (mode & 0100u) ? 'x' : '-';
    out[4] = (mode & 0040u) ? 'r' : '-';
    out[5] = (mode & 0020u) ? 'w' : '-';
    out[6] = (mode & 0010u) ? 'x' : '-';
    out[7] = (mode & 0004u) ? 'r' : '-';
    out[8] = (mode & 0002u) ? 'w' : '-';
    out[9] = (mode & 0001u) ? 'x' : '-';
    out[10] = '\0';
}

int main(int argc, char **argv)
{
    int long_flag = 0;
    int all_flag  = 0;
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            for (char *p = argv[i] + 1; *p; p++) {
                if (*p == 'l') long_flag = 1;
                if (*p == 'a') all_flag  = 1;
            }
        } else {
            path = argv[i];
        }
    }

    if (!path) {
        if (getcwd(cwd_buf, sizeof(cwd_buf)))
            path = cwd_buf;
        else
            path = "/";
    }

    long n = all_flag
        ? listdir_all(path, ls_buf, (long)sizeof(ls_buf))
        : listdir(path, ls_buf, (long)sizeof(ls_buf));

    if (n < 0) {
        printf("ls: cannot open directory: %s\n", path);
        return 1;
    }

    if (!long_flag) {
        write(1, ls_buf, (size_t)n);
        return 0;
    }

    /* Long format */
    int plen = (int)strlen(path);
    char full_path[512];
    char mode_str[11];
    char *p = ls_buf;

    while (*p) {
        char *nl = p;
        while (*nl && *nl != '\n') nl++;
        int namelen = (int)(nl - p);
        if (namelen == 0) { p = (*nl == '\n') ? nl + 1 : nl; continue; }

        /* Build absolute path for stat */
        int base = plen;
        if (base > 0 && path[base - 1] == '/') base--;
        if (base + 1 + namelen < (int)sizeof(full_path)) {
            memcpy(full_path, path, (size_t)base);
            full_path[base] = '/';
            memcpy(full_path + base + 1, p, (size_t)namelen);
            full_path[base + 1 + namelen] = '\0';
        } else {
            full_path[0] = '\0';
        }

        struct stat st;
        if (stat(full_path, &st) == 0) {
            fmt_mode(st.st_mode, mode_str);
            printf("%s", mode_str);
            print_rjust(st.st_nlink, 4);
            putchar(' ');
            print_rjust(st.st_uid, 4);
            putchar(' ');
            print_rjust(st.st_gid, 4);
            putchar(' ');
            print_rjust(st.st_size, 8);
            putchar(' ');
        } else {
            printf("??????????   ?    ?    ?        ? ");
        }

        write(1, p, (size_t)namelen);
        write(1, "\n", 1);

        p = (*nl == '\n') ? nl + 1 : nl;
    }
    return 0;
}
