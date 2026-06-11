#ifndef OS_BOOTINFO_H
#define OS_BOOTINFO_H

#include <stdint.h>

#define OS_BOOTINFO_MAGIC 0x4f53424f4f54494eULL
#define OS_BOOTINFO_VERSION 1U

#define OS_KERNEL_VMA_BASE 0xffffffff80000000ULL
#define OS_KERNEL_LMA_BASE 0x0000000000200000ULL
#define OS_DIRECT_MAP_BASE 0xffff800000000000ULL
#define OS_EARLY_DIRECT_MAP_SIZE (64ULL * 1024ULL * 1024ULL * 1024ULL)

#define OS_PAGE_SIZE 4096ULL

enum os_framebuffer_format {
	OS_FB_FORMAT_RGB = 0,
	OS_FB_FORMAT_BGR = 1,
	OS_FB_FORMAT_BITMASK = 2,
	OS_FB_FORMAT_UNKNOWN = 0xffffffffU,
};

struct os_framebuffer_info {
	uint64_t base;
	uint32_t width;
	uint32_t height;
	uint32_t pixels_per_scanline;
	uint32_t format;
	uint32_t red_mask;
	uint32_t green_mask;
	uint32_t blue_mask;
	uint32_t reserved_mask;
};

struct os_boot_info {
	uint64_t magic;
	uint32_t version;
	uint32_t size;
	uint64_t flags;

	uint64_t rsdp;

	uint64_t memory_map;
	uint64_t memory_map_size;
	uint64_t memory_descriptor_size;
	uint32_t memory_descriptor_version;
	uint32_t reserved0;

	struct os_framebuffer_info framebuffer;

	uint64_t kernel_phys_start;
	uint64_t kernel_phys_end;
	uint64_t kernel_virt_start;
	uint64_t kernel_virt_end;

	uint64_t direct_map_base;
	uint64_t direct_map_size;
	uint64_t page_table_root;

	/* Physical extent of the pages the bootloader allocated that the kernel
	   continues to use after ExitBootServices: the page tables, this boot_info
	   struct, and the UEFI memory map.  These are EFI_LOADER_DATA and would
	   otherwise be reclaimed as free RAM by the PMM, which would then hand out
	   the kernel's own live page tables.  The PMM marks [start, end) used. */
	uint64_t loader_reserved_start;
	uint64_t loader_reserved_end;
};

struct os_uefi_memory_descriptor {
	uint32_t type;
	uint32_t pad;
	uint64_t physical_start;
	uint64_t virtual_start;
	uint64_t number_of_pages;
	uint64_t attribute;
};

enum os_uefi_memory_type {
	OS_EFI_RESERVED_MEMORY_TYPE = 0,
	OS_EFI_LOADER_CODE = 1,
	OS_EFI_LOADER_DATA = 2,
	OS_EFI_BOOT_SERVICES_CODE = 3,
	OS_EFI_BOOT_SERVICES_DATA = 4,
	OS_EFI_RUNTIME_SERVICES_CODE = 5,
	OS_EFI_RUNTIME_SERVICES_DATA = 6,
	OS_EFI_CONVENTIONAL_MEMORY = 7,
	OS_EFI_UNUSABLE_MEMORY = 8,
	OS_EFI_ACPI_RECLAIM_MEMORY = 9,
	OS_EFI_ACPI_MEMORY_NVS = 10,
	OS_EFI_MEMORY_MAPPED_IO = 11,
	OS_EFI_MEMORY_MAPPED_IO_PORT_SPACE = 12,
	OS_EFI_PAL_CODE = 13,
	OS_EFI_PERSISTENT_MEMORY = 14,
};

#endif

