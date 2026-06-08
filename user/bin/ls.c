#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "/";
    char buf[4096];
    long n = listdir(path, buf, (long)sizeof(buf));
    if (n < 0) {
        printf("ls: cannot open directory: %s\n", path);
        return 1;
    }
    write(1, buf, (size_t)n);
    return 0;
}
