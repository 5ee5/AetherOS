#include <stdio.h>
#include <unistd.h>

static void print_size(uint64_t bytes)
{
    if (bytes >= 1024UL * 1024 * 1024)
        printf("%luG", (unsigned long)(bytes / (1024UL * 1024 * 1024)));
    else if (bytes >= 1024UL * 1024)
        printf("%luM", (unsigned long)(bytes / (1024UL * 1024)));
    else
        printf("%luK", (unsigned long)(bytes / 1024UL));
}

int main(void)
{
    uint64_t total, free_disk;
    if (diskinfo(&total, &free_disk) < 0) {
        puts("df: failed to get disk info");
        return 1;
    }
    uint64_t used = total - free_disk;
    uint64_t pct  = total ? used * 100 / total : 0;
    printf("Filesystem       Size     Used     Free  Use%%  Mounted on\n");
    printf("/dev/sda0   ");
    print_size(total); print_size(used); print_size(free_disk);
    printf("  %lu%%  /\n", (unsigned long)pct);
    return 0;
}
