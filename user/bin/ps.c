#include <stdio.h>
#include <unistd.h>

static char buf[4096];

int main(void)
{
    ps_list(buf, sizeof(buf));
    puts(buf);
    return 0;
}
