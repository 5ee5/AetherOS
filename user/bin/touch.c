#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 2) { puts("usage: touch <file>"); return 1; }
    for (int i = 1; i < argc; i++) {
        if (creat(argv[i]) < 0)
            printf("touch: %s: failed\n", argv[i]);
    }
    return 0;
}
