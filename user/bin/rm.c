#include <unistd.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    if (argc < 2) { puts("usage: rm <path>"); return 1; }
    if (unlink(argv[1]) < 0) { puts("rm: failed"); return 1; }
    return 0;
}
