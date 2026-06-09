#include <stdio.h>
#include <unistd.h>

static void print_size(uint64_t bytes)
{
    if (bytes >= 1024ULL * 1024 * 1024)
        printf("%4lluG", (unsigned long long)(bytes / (1024ULL * 1024 * 1024)));
    else if (bytes >= 1024ULL * 1024)
        printf("%4lluM", (unsigned long long)(bytes / (1024ULL * 1024)));
    else
        printf("%4lluK", (unsigned long long)(bytes / 1024ULL));
}

int main(void)
{
    uint64_t total, free_mem;
    if (meminfo(&total, &free_mem) < 0) {
        puts("free: failed to get memory info");
        return 1;
    }
    uint64_t used = total - free_mem;
    printf("       total    used    free\n");
    printf("Mem:   ");
    print_size(total); putchar(' ');
    print_size(used);  putchar(' ');
    print_size(free_mem);
    putchar('\n');
    return 0;
}
