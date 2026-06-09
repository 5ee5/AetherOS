#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        puts("usage: cat <file> [...]");
        return 1;
    }
    char buf[4096];
    int ret = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], 0, 0);
        if (fd < 0) {
            printf("cat: %s: no such file\n", argv[i]);
            ret = 1;
            continue;
        }
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0)
            write(1, buf, (size_t)n);
        close(fd);
    }
    return ret;
}
