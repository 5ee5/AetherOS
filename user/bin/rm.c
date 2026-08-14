#include <unistd.h>
#include <stdio.h>
#include <sys/stat.h>

int main(int argc, char **argv)
{
    if (argc < 2) { puts("usage: rm <path>..."); return 1; }

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        struct stat st;
        /* Directories are refused rather than unlinked: removing one through
           unlink() leaves "." and ".." behind and corrupts the filesystem. */
        if (stat(argv[i], &st) == 0 && S_ISDIR(st.st_mode)) {
            printf("rm: %s: is a directory (use rmdir)\n", argv[i]);
            rc = 1;
            continue;
        }
        if (unlink(argv[i]) < 0) {
            printf("rm: %s: failed\n", argv[i]);
            rc = 1;
        }
    }
    return rc;
}
