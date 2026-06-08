#include <unistd.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    if (argc < 2) { puts("usage: mkdir <path>"); return 1; }
    if (mkdir(argv[1], 0755) < 0) { puts("mkdir: failed"); return 1; }
    return 0;
}
