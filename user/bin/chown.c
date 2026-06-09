#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

int main(int argc, char **argv)
{
    if (argc < 3) {
        puts("usage: chown <uid>[:<gid>] <file> [...]");
        return 1;
    }

    char *colon = strchr(argv[1], ':');
    uid_t new_uid = (uid_t)atoi(argv[1]);
    int   has_gid = (colon != 0);
    gid_t new_gid = has_gid ? (gid_t)atoi(colon + 1) : 0;

    int ret = 0;
    for (int i = 2; i < argc; i++) {
        gid_t use_gid = new_gid;
        if (!has_gid) {
            struct stat st;
            if (stat(argv[i], &st) == 0)
                use_gid = (gid_t)st.st_gid;
        }
        if (chown(argv[i], new_uid, use_gid) < 0) {
            printf("chown: %s: failed\n", argv[i]);
            ret = 1;
        }
    }
    return ret;
}
