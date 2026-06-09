#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 2) { puts("usage: sleep <seconds>"); return 1; }
    long secs = 0;
    for (char *p = argv[1]; *p >= '0' && *p <= '9'; p++)
        secs = secs * 10 + (*p - '0');
    sleep_ms(secs * 1000L);
    return 0;
}
