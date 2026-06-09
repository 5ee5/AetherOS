#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 2) { puts("usage: kill <pid>"); return 1; }
    int pid = 0;
    for (char *p = argv[1]; *p >= '0' && *p <= '9'; p++)
        pid = pid * 10 + (*p - '0');
    if (kill(pid) < 0) {
        printf("kill: no process %d\n", pid);
        return 1;
    }
    return 0;
}
