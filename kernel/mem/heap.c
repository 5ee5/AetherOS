#include "mem/heap.h"

#include <stddef.h>
#include <stdint.h>

#include "core/panic.h"
#include "core/serial.h"
#include "mem/pmm.h"
#include "mem/vmm.h"
#include "os/bootinfo.h"
#include "sync/spinlock.h"

/* IRQ-safe: kfree() is reached from the timer ISR's deferred reaper, so a
   thread preempted mid-allocation must not deadlock the ISR on the same CPU. */
static spinlock_t heap_lock = SPINLOCK_INIT;

/* First virtual page of the kernel heap — well above the 64 GiB direct map. */
#define HEAP_VIRT_BASE 0xffff900000000000ULL
#define HEAP_MAGIC     0xc0de4ea1UL
#define BLOCK_FREE     0U
#define BLOCK_USED     1U
/* Minimum leftover size to justify splitting a free block. */
#define MIN_SPLIT      (sizeof(struct block) + 16U)

struct block {
	uint32_t magic;
	uint32_t flags;
	uint64_t size;         /* total block size including this header */
	struct block *prev;    /* previous block in address order */
	struct block *next_free;
	struct block *prev_free;
};

static struct block *free_list;
static struct block *heap_last;
static uint64_t heap_end;

static void free_list_add(struct block *b)
{
	b->next_free = free_list;
	b->prev_free = NULL;
	if (free_list) {
		free_list->prev_free = b;
	}
	free_list = b;
}

static void free_list_remove(struct block *b)
{
	if (b->prev_free) {
		b->prev_free->next_free = b->next_free;
	} else {
		free_list = b->next_free;
	}
	if (b->next_free) {
		b->next_free->prev_free = b->prev_free;
	}
	b->next_free = NULL;
	b->prev_free = NULL;
}

/* Returns the next block in address order, or NULL if b is the last. */
static struct block *block_next_mem(struct block *b)
{
	struct block *n = (struct block *)((char *)b + b->size);
	if ((uint64_t)(uintptr_t)n >= heap_end) {
		return NULL;
	}
	return n;
}

static bool expand(void)
{
	uint64_t page = pmm_alloc_page();
	if (page == PMM_ALLOC_FAILED) {
		return false;
	}
	if (!vmm_map(heap_end, page, VMM_WRITABLE)) {
		pmm_free_page(page);
		return false;
	}

	struct block *b = (struct block *)(uintptr_t)heap_end;
	heap_end += OS_PAGE_SIZE;

	if (heap_last && heap_last->flags == BLOCK_FREE) {
		/* Coalesce: extend the trailing free block into the new page. */
		free_list_remove(heap_last);
		heap_last->size += OS_PAGE_SIZE;
		free_list_add(heap_last);
	} else {
		b->magic     = HEAP_MAGIC;
		b->flags     = BLOCK_FREE;
		b->size      = OS_PAGE_SIZE;
		b->prev      = heap_last;
		b->next_free = NULL;
		b->prev_free = NULL;
		heap_last    = b;
		free_list_add(b);
	}
	return true;
}

void heap_init(void)
{
	heap_end  = HEAP_VIRT_BASE;
	heap_last = NULL;
	free_list = NULL;

	if (!expand()) {
		panic("heap: initial page allocation failed");
	}

	serial_write("heap: base=");
	serial_write_hex(HEAP_VIRT_BASE);
	serial_write("\n");
}

/* Core allocator; caller must hold heap_lock. */
static void *kmalloc_locked(uint64_t size)
{
	size = (size + 15U) & ~15ULL;  /* 16-byte alignment */
	uint64_t total = size + sizeof(struct block);

	/* First fit. */
	struct block *b = free_list;
	while (b && b->size < total) {
		b = b->next_free;
	}

	if (!b) {
		uint64_t pages = (total + OS_PAGE_SIZE - 1U) / OS_PAGE_SIZE;
		for (uint64_t i = 0; i < pages; ++i) {
			if (!expand()) {
				return NULL;
			}
		}
		b = free_list;
		while (b && b->size < total) {
			b = b->next_free;
		}
		if (!b) {
			return NULL;
		}
	}

	if (b->size >= total + MIN_SPLIT) {
		struct block *tail = (struct block *)((char *)b + total);
		tail->magic     = HEAP_MAGIC;
		tail->flags     = BLOCK_FREE;
		tail->size      = b->size - total;
		tail->prev      = b;
		tail->next_free = NULL;
		tail->prev_free = NULL;

		struct block *after = block_next_mem(tail);
		if (after) {
			after->prev = tail;
		}
		if (heap_last == b) {
			heap_last = tail;
		}
		b->size = total;
		free_list_remove(b);
		free_list_add(tail);
	} else {
		free_list_remove(b);
	}

	b->flags = BLOCK_USED;
	return (void *)((char *)b + sizeof(struct block));
}

void *kmalloc(uint64_t size)
{
	if (size == 0) {
		return NULL;
	}
	uint64_t flags = spinlock_acquire_irqsave(&heap_lock);
	void *p = kmalloc_locked(size);
	spinlock_release_irqrestore(&heap_lock, flags);
	return p;
}

/* Core free; caller must hold heap_lock. */
static void kfree_locked(void *ptr)
{
	struct block *b = (struct block *)((char *)ptr - sizeof(struct block));
	if (b->magic != HEAP_MAGIC || b->flags != BLOCK_USED) {
		panic("heap: invalid free");
	}
	b->flags = BLOCK_FREE;

	/* Coalesce with next block if free. */
	struct block *n = block_next_mem(b);
	if (n && n->magic == HEAP_MAGIC && n->flags == BLOCK_FREE) {
		free_list_remove(n);
		b->size += n->size;
		if (heap_last == n) {
			heap_last = b;
		}
		struct block *after = block_next_mem(b);
		if (after) {
			after->prev = b;
		}
	}

	/* Coalesce with previous block if free. */
	struct block *p = b->prev;
	if (p && p->magic == HEAP_MAGIC && p->flags == BLOCK_FREE) {
		free_list_remove(p);
		p->size += b->size;
		if (heap_last == b) {
			heap_last = p;
		}
		struct block *after = block_next_mem(p);
		if (after) {
			after->prev = p;
		}
		b = p;
	}

	free_list_add(b);
}

void kfree(void *ptr)
{
	if (!ptr) {
		return;
	}
	uint64_t flags = spinlock_acquire_irqsave(&heap_lock);
	kfree_locked(ptr);
	spinlock_release_irqrestore(&heap_lock, flags);
}
