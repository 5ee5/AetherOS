#include <stdlib.h>
#include <stddef.h>

#define HEAP_SZ (256 * 1024)
static char   s_heap[HEAP_SZ];
static size_t s_ptr;

void *malloc(size_t n)
{
    n = (n + 15UL) & ~15UL;
    if (s_ptr + n > HEAP_SZ) return (void *)0;
    void *p = s_heap + s_ptr;
    s_ptr += n;
    return p;
}

void free(void *p)
{
    (void)p;
}

int atoi(const char *s)
{
    int n = 0, neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
    return neg ? -n : n;
}
