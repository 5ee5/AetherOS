#include <stdlib.h>
#include <stddef.h>

/* Free-list allocator over a static 256 KiB arena.
   Each block has a header: [size_t total_size | size_t in_use]
   `total_size` includes the header itself. `in_use` is 0 or 1. */

#define HEAP_SZ    (256 * 1024)
#define HDR_SZ     (2 * sizeof(size_t))
#define ALIGN      16UL
#define ALIGN_UP(n) (((n) + ALIGN - 1) & ~(ALIGN - 1))

static char s_heap[HEAP_SZ];
static int  s_init;

typedef struct blk {
    size_t total; /* size of this block in bytes, including header */
    size_t used;  /* 1 = allocated, 0 = free */
} blk_t;

static void heap_init(void)
{
    blk_t *b = (blk_t *)s_heap;
    b->total = HEAP_SZ;
    b->used  = 0;
    s_init   = 1;
}

void *malloc(size_t n)
{
    if (!s_init) heap_init();
    if (n == 0) return (void *)0;

    size_t need = ALIGN_UP(n + HDR_SZ);
    if (need < HDR_SZ + ALIGN) need = HDR_SZ + ALIGN;

    char *p = s_heap;
    char *end = s_heap + HEAP_SZ;

    while (p < end) {
        blk_t *b = (blk_t *)p;
        if (!b->used && b->total >= need) {
            /* Split if there's room for a second free block. */
            if (b->total >= need + HDR_SZ + ALIGN) {
                blk_t *rest = (blk_t *)(p + need);
                rest->total = b->total - need;
                rest->used  = 0;
                b->total    = need;
            }
            b->used = 1;
            return (void *)(p + HDR_SZ);
        }
        p += b->total;
    }
    return (void *)0;
}

void free(void *ptr)
{
    if (!ptr) return;
    blk_t *b = (blk_t *)((char *)ptr - HDR_SZ);
    b->used = 0;

    /* Coalesce forward. */
    char *next = (char *)b + b->total;
    char *end  = s_heap + HEAP_SZ;
    while (next < end) {
        blk_t *nb = (blk_t *)next;
        if (nb->used) break;
        b->total += nb->total;
        next      = (char *)b + b->total;
    }
}

void *calloc(size_t nmemb, size_t size)
{
    size_t n = nmemb * size;
    void *p = malloc(n);
    if (p) {
        char *c = (char *)p;
        for (size_t i = 0; i < n; i++) c[i] = 0;
    }
    return p;
}

void *realloc(void *ptr, size_t size)
{
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return (void *)0; }
    blk_t *b = (blk_t *)((char *)ptr - HDR_SZ);
    size_t cur = b->total - HDR_SZ;
    if (cur >= size) return ptr;
    void *np = malloc(size);
    if (!np) return (void *)0;
    char *src = (char *)ptr;
    char *dst = (char *)np;
    for (size_t i = 0; i < cur; i++) dst[i] = src[i];
    free(ptr);
    return np;
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
