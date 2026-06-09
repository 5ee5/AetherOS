#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 3) { puts("usage: chmod <octal-mode> <file>"); return 1; }
    int mode = 0;
    for (char *p = argv[1]; *p >= '0' && *p <= '7'; p++)
        mode = mode * 8 + (*p - '0');
    if (chmod(argv[2], mode) < 0) {
        puts("chmod: permission denied or file not found");
        return 1;
    }
    return 0;
}
