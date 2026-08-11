#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>

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
    /* setuid/setgid/sticky replace the matching execute slot, as ls does: a
       lowercase letter when execute is also set, uppercase when it is not.
       Without this a setuid binary was indistinguishable from a plain one. */
    if (mode & 04000u) out[3] = (mode & 0100u) ? 's' : 'S';
    if (mode & 02000u) out[6] = (mode & 0010u) ? 's' : 'S';
    if (mode & 01000u) out[9] = (mode & 0001u) ? 't' : 'T';
    out[10] = '\0';
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        puts("usage: stat <file> [...]");
        return 1;
    }
    int ret = 0;
    for (int i = 1; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) < 0) {
            printf("stat: %s: no such file\n", argv[i]);
            ret = 1;
            continue;
        }
        char mode_str[11];
        fmt_mode(st.st_mode, mode_str);
        printf("  File: %s\n", argv[i]);
        printf("  Size: %u\n", st.st_size);
        printf(" Links: %u\n", (unsigned)st.st_nlink);
        printf("  Mode: %s (%04o)\n", mode_str, (unsigned)(st.st_mode & 0xfffU));
        printf("   Uid: %u\n", st.st_uid);
        printf("   Gid: %u\n", st.st_gid);
    }
    return ret;
}
