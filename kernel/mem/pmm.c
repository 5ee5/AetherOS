#include "mem/pmm.h"

#include <stddef.h>
#include <stdint.h>

#include "core/panic.h"
#include "core/serial.h"
#include "sync/spinlock.h"

/* IRQ-safe: reached from the heap's expand() which can run in the timer ISR. */
static spinlock_t pmm_lock = SPINLOCK_INIT;

#define PMM_MAX_PHYS_BYTES OS_EARLY_DIRECT_MAP_SIZE
#define PMM_MAX_PAGES (PMM_MAX_PHYS_BYTES / OS_PAGE_SIZE)
#define BITMAP_WORD_BITS 64ULL
#define PMM_BITMAP_WORDS (PMM_MAX_PAGES / BITMAP_WORD_BITS)

static uint64_t pmm_bitmap[PMM_BITMAP_WORDS];
static uint64_t pmm_total;
static uint64_t pmm_free;

static void bitmap_set(uint64_t page)
{
	pmm_bitmap[page / BITMAP_WORD_BITS] |= 1ULL << (page % BITMAP_WORD_BITS);
}

static void bitmap_clear(uint64_t page)
{
	pmm_bitmap[page / BITMAP_WORD_BITS] &= ~(1ULL <<
		(page % BITMAP_WORD_BITS));
}

static bool bitmap_test(uint64_t page)
{
	return (pmm_bitmap[page / BITMAP_WORD_BITS] &
		(1ULL << (page % BITMAP_WORD_BITS))) != 0;
}

static void mark_range_free(uint64_t start, uint64_t end)
{
	if (start >= PMM_MAX_PHYS_BYTES) {
		return;
	}
	if (end > PMM_MAX_PHYS_BYTES) {
		end = PMM_MAX_PHYS_BYTES;
	}

	uint64_t first_page = (start + OS_PAGE_SIZE - 1ULL) / OS_PAGE_SIZE;
	uint64_t last_page = end / OS_PAGE_SIZE;
	for (uint64_t page = first_page; page < last_page; ++page) {
		if (bitmap_test(page)) {
			bitmap_clear(page);
			++pmm_free;
		}
	}
}

static void mark_range_used(uint64_t start, uint64_t end)
{
	if (start >= PMM_MAX_PHYS_BYTES) {
		return;
	}
	if (end > PMM_MAX_PHYS_BYTES) {
		end = PMM_MAX_PHYS_BYTES;
	}

	uint64_t first_page = start / OS_PAGE_SIZE;
	uint64_t last_page = (end + OS_PAGE_SIZE - 1ULL) / OS_PAGE_SIZE;
	for (uint64_t page = first_page; page < last_page; ++page) {
		if (!bitmap_test(page)) {
			bitmap_set(page);
			--pmm_free;
		}
	}
}

void pmm_init(const struct os_boot_info *boot_info)
{
	for (uint64_t i = 0; i < PMM_BITMAP_WORDS; ++i) {
		pmm_bitmap[i] = UINT64_MAX;
	}
	pmm_total = PMM_MAX_PAGES;
	pmm_free = 0;

	const uint8_t *map = (const uint8_t *)(uintptr_t)boot_info->memory_map;
	for (uint64_t offset = 0; offset < boot_info->memory_map_size;
		offset += boot_info->memory_descriptor_size) {
		const struct os_uefi_memory_descriptor *desc =
			(const struct os_uefi_memory_descriptor *)(map + offset);
		/* EFI_LOADER_DATA is deliberately NOT reclaimed: the bootloader
		   allocates the kernel's page tables, boot_info, and the UEFI
		   memory map there, and the kernel keeps using them after
		   ExitBootServices.  Freeing them would let the PMM hand out the
		   kernel's own live page tables. LOADER_CODE (the bootloader image)
		   and boot-services memory are safe to reclaim. */
		if (desc->type == OS_EFI_CONVENTIONAL_MEMORY ||
			desc->type == OS_EFI_LOADER_CODE ||
			desc->type == OS_EFI_BOOT_SERVICES_CODE ||
			desc->type == OS_EFI_BOOT_SERVICES_DATA) {
			uint64_t start = desc->physical_start;
			uint64_t end = start + desc->number_of_pages * OS_PAGE_SIZE;
			mark_range_free(start, end);
		}
	}

	mark_range_used(0, 0x100000ULL);
	mark_range_used(boot_info->kernel_phys_start,
		boot_info->kernel_phys_end);
	mark_range_used(boot_info->page_table_root,
		boot_info->page_table_root + OS_PAGE_SIZE);

	serial_write("pmm: total_pages=");
	serial_write_dec(pmm_total);
	serial_write(" free_pages=");
	serial_write_dec(pmm_free);
	serial_write("\n");

	if (pmm_free == 0) {
		panic("PMM found no free pages");
	}
}

uint64_t pmm_alloc_page(void)
{
	uint64_t flags = spinlock_acquire_irqsave(&pmm_lock);
	for (uint64_t word = 0; word < PMM_BITMAP_WORDS; ++word) {
		if (pmm_bitmap[word] == UINT64_MAX) {
			continue;
		}
		uint64_t bit = (uint64_t)__builtin_ctzll(~pmm_bitmap[word]);
		uint64_t page = word * BITMAP_WORD_BITS + bit;
		pmm_bitmap[word] |= 1ULL << bit;
		--pmm_free;
		spinlock_release_irqrestore(&pmm_lock, flags);
		return page * OS_PAGE_SIZE;
	}
	spinlock_release_irqrestore(&pmm_lock, flags);
	return PMM_ALLOC_FAILED;
}

void pmm_free_page(uint64_t physical_address)
{
	if ((physical_address % OS_PAGE_SIZE) != 0 ||
		physical_address >= PMM_MAX_PHYS_BYTES) {
		panic("invalid PMM free");
	}

	uint64_t page = physical_address / OS_PAGE_SIZE;
	uint64_t flags = spinlock_acquire_irqsave(&pmm_lock);
	if (!bitmap_test(page)) {
		spinlock_release_irqrestore(&pmm_lock, flags);
		panic("double PMM free");
	}
	bitmap_clear(page);
	++pmm_free;
	spinlock_release_irqrestore(&pmm_lock, flags);
}

uint64_t pmm_total_pages(void)
{
	return pmm_total;
}

uint64_t pmm_free_pages(void)
{
	return pmm_free;
}

bool pmm_address_in_range(uint64_t physical_address)
{
	return physical_address < PMM_MAX_PHYS_BYTES;
}

