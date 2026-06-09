#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static const char *basename_of(const char *path)
{
    const char *name = path;
    for (const char *p = path; *p; p++)
        if (*p == '/') name = p + 1;
    return name;
}

static int move_into_dir(const char *src, const char *dst_dir)
{
    char full_dst[512];
    const char *name = basename_of(src);
    int dlen = (int)strlen(dst_dir);
    if (dlen + 1 + (int)strlen(name) + 1 > (int)sizeof(full_dst)) {
        printf("mv: path too long: %s\n", src);
        return 1;
    }
    memcpy(full_dst, dst_dir, (size_t)dlen);
    if (full_dst[dlen - 1] != '/') full_dst[dlen++] = '/';
    strcpy(full_dst + dlen, name);
    if (rename(src, full_dst) < 0) {
        printf("mv: cannot move '%s' to '%s'\n", src, full_dst);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        puts("usage: mv <source> <dest>");
        puts("       mv <source...> <directory>");
        return 1;
    }

    const char *dst = argv[argc - 1];
    struct stat st;
    int dst_is_dir = (stat(dst, &st) == 0 && S_ISDIR(st.st_mode));

    if (argc == 3 && !dst_is_dir) {
        /* Simple rename / cross-directory move. */
        if (rename(argv[1], dst) < 0) {
            printf("mv: cannot move '%s' to '%s'\n", argv[1], dst);
            return 1;
        }
        return 0;
    }

    /* One or more sources moving into a directory. */
    if (!dst_is_dir) {
        puts("mv: destination must be a directory for multiple sources");
        return 1;
    }

    int ret = 0;
    for (int i = 1; i < argc - 1; i++)
        ret |= move_into_dir(argv[i], dst);
    return ret;
}
