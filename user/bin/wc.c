#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int fd = 0;
    if (argc > 1) {
        fd = open(argv[1], 0, 0);
        if (fd < 0) {
            printf("wc: %s: not found\n", argv[1]);
            return 1;
        }
    }
    long lines = 0, words = 0, bytes = 0;
    int  inword = 0;
    char buf[256];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        bytes += n;
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') lines++;
            if (c == ' ' || c == '\n' || c == '\t') {
                inword = 0;
            } else if (!inword) {
                inword = 1;
                words++;
            }
        }
    }
    if (argc > 1) close(fd);
    printf("%ld %ld %ld\n", lines, words, bytes);
    return 0;
}
