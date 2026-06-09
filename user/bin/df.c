#include <stdio.h>
#include <unistd.h>

static void print_size(uint64_t bytes)
{
    if (bytes >= 1024ULL * 1024 * 1024)
        printf("%8lluG", (unsigned long long)(bytes / (1024ULL * 1024 * 1024)));
    else if (bytes >= 1024ULL * 1024)
        printf("%8lluM", (unsigned long long)(bytes / (1024ULL * 1024)));
    else
        printf("%8lluK", (unsigned long long)(bytes / 1024ULL));
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
    printf("  %3llu%%  /\n", (unsigned long long)pct);
    return 0;
}
