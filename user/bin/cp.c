#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc < 3) { puts("usage: cp <src> <dst>"); return 1; }

    int src = open(argv[1], O_RDONLY, 0);
    if (src < 0) { puts("cp: cannot open source"); return 1; }

    int dst = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst < 0) { close(src); puts("cp: cannot open dest"); return 1; }

    char buf[512];
    long n;
    while ((n = read(src, buf, sizeof(buf))) > 0)
        write(dst, buf, (size_t)n);

    close(src);
    close(dst);
    return 0;
}
