#include <unistd.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    if (argc < 2) { puts("usage: rmdir <dir>..."); return 1; }

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (rmdir(argv[i]) < 0) {
            printf("rmdir: %s: not an empty directory\n", argv[i]);
            rc = 1;
        }
    }
    return rc;
}
