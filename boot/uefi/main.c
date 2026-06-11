#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "os/bootinfo.h"
#include "os/elf64.h"

#define EFIAPI __attribute__((ms_abi))

#define EFI_SUCCESS 0ULL
#define EFI_BUFFER_TOO_SMALL 0x8000000000000005ULL

#define EFI_ERROR(status) (((status) & 0x8000000000000000ULL) != 0)

#define EFI_FILE_MODE_READ 0x0000000000000001ULL

#define EFI_ALLOCATE_ANY_PAGES 0
#define EFI_ALLOCATE_MAX_ADDRESS 1
#define EFI_ALLOCATE_ADDRESS 2

#define EFI_LOADER_DATA 2

#define PAGE_PRESENT 0x001ULL
#define PAGE_WRITABLE 0x002ULL
#define PAGE_LARGE 0x080ULL

#define PAGE_MASK 0x000ffffffffff000ULL
#define TWO_MIB (2ULL * 1024ULL * 1024ULL)

typedef uint16_t efi_char16_t;
typedef uint64_t efi_physical_address_t;
typedef uint64_t efi_virtual_address_t;
typedef uint64_t efi_status_t;
typedef uint64_t efi_uintn_t;
typedef uint32_t efi_uint32_t;
typedef void *efi_handle_t;
typedef void *efi_event_t;

struct efi_guid {
	uint32_t data1;
	uint16_t data2;
	uint16_t data3;
	uint8_t data4[8];
};

struct efi_table_header {
	uint64_t signature;
	uint32_t revision;
	uint32_t header_size;
	uint32_t crc32;
	uint32_t reserved;
};

struct efi_simple_text_output_protocol {
	void *reset;
	efi_status_t(EFIAPI *output_string)(
		struct efi_simple_text_output_protocol *self,
		const efi_char16_t *string);
};

struct efi_configuration_table {
	struct efi_guid vendor_guid;
	void *vendor_table;
};

struct efi_boot_services {
	struct efi_table_header hdr;
	void *raise_tpl;
	void *restore_tpl;
	efi_status_t(EFIAPI *allocate_pages)(uint32_t type, uint32_t memory_type,
		efi_uintn_t pages, efi_physical_address_t *memory);
	efi_status_t(EFIAPI *free_pages)(efi_physical_address_t memory,
		efi_uintn_t pages);
	efi_status_t(EFIAPI *get_memory_map)(efi_uintn_t *memory_map_size,
		void *memory_map, efi_uintn_t *map_key,
		efi_uintn_t *descriptor_size, uint32_t *descriptor_version);
	efi_status_t(EFIAPI *allocate_pool)(uint32_t pool_type, efi_uintn_t size,
		void **buffer);
	efi_status_t(EFIAPI *free_pool)(void *buffer);
	void *create_event;
	void *set_timer;
	void *wait_for_event;
	void *signal_event;
	void *close_event;
	void *check_event;
	void *install_protocol_interface;
	void *reinstall_protocol_interface;
	void *uninstall_protocol_interface;
	efi_status_t(EFIAPI *handle_protocol)(efi_handle_t handle,
		const struct efi_guid *protocol, void **interface);
	void *reserved;
	void *register_protocol_notify;
	void *locate_handle;
	void *locate_device_path;
	void *install_configuration_table;
	void *load_image;
	void *start_image;
	void *exit;
	void *unload_image;
	efi_status_t(EFIAPI *exit_boot_services)(efi_handle_t image_handle,
		efi_uintn_t map_key);
	void *get_next_monotonic_count;
	void *stall;
	efi_status_t(EFIAPI *set_watchdog_timer)(efi_uintn_t timeout,
		uint64_t watchdog_code, efi_uintn_t data_size,
		efi_char16_t *watchdog_data);
	void *connect_controller;
	void *disconnect_controller;
	void *open_protocol;
	void *close_protocol;
	void *open_protocol_information;
	void *protocols_per_handle;
	void *locate_handle_buffer;
	efi_status_t(EFIAPI *locate_protocol)(const struct efi_guid *protocol,
		void *registration, void **interface);
	void *install_multiple_protocol_interfaces;
	void *uninstall_multiple_protocol_interfaces;
	void *calculate_crc32;
	void *copy_mem;
	void *set_mem;
	void *create_event_ex;
};

struct efi_system_table {
	struct efi_table_header hdr;
	efi_char16_t *firmware_vendor;
	uint32_t firmware_revision;
	efi_handle_t console_in_handle;
	void *con_in;
	efi_handle_t console_out_handle;
	struct efi_simple_text_output_protocol *con_out;
	efi_handle_t standard_error_handle;
	struct efi_simple_text_output_protocol *std_err;
	void *runtime_services;
	struct efi_boot_services *boot_services;
	efi_uintn_t number_of_table_entries;
	struct efi_configuration_table *configuration_table;
};

struct efi_loaded_image_protocol {
	uint32_t revision;
	efi_handle_t parent_handle;
	struct efi_system_table *system_table;
	efi_handle_t device_handle;
	void *file_path;
	void *reserved;
	uint32_t load_options_size;
	void *load_options;
	void *image_base;
	uint64_t image_size;
	uint32_t image_code_type;
	uint32_t image_data_type;
	efi_status_t(EFIAPI *unload)(efi_handle_t image_handle);
};

struct efi_file_protocol {
	uint64_t revision;
	efi_status_t(EFIAPI *open)(struct efi_file_protocol *self,
		struct efi_file_protocol **new_handle, const efi_char16_t *file_name,
		uint64_t open_mode, uint64_t attributes);
	efi_status_t(EFIAPI *close)(struct efi_file_protocol *self);
	efi_status_t(EFIAPI *delete)(struct efi_file_protocol *self);
	efi_status_t(EFIAPI *read)(struct efi_file_protocol *self,
		efi_uintn_t *buffer_size, void *buffer);
	efi_status_t(EFIAPI *write)(struct efi_file_protocol *self,
		efi_uintn_t *buffer_size, void *buffer);
	efi_status_t(EFIAPI *get_position)(struct efi_file_protocol *self,
		uint64_t *position);
	efi_status_t(EFIAPI *set_position)(struct efi_file_protocol *self,
		uint64_t position);
	efi_status_t(EFIAPI *get_info)(struct efi_file_protocol *self,
		const struct efi_guid *information_type, efi_uintn_t *buffer_size,
		void *buffer);
	efi_status_t(EFIAPI *set_info)(struct efi_file_protocol *self,
		const struct efi_guid *information_type, efi_uintn_t buffer_size,
		void *buffer);
	efi_status_t(EFIAPI *flush)(struct efi_file_protocol *self);
};

struct efi_simple_file_system_protocol {
	uint64_t revision;
	efi_status_t(EFIAPI *open_volume)(
		struct efi_simple_file_system_protocol *self,
		struct efi_file_protocol **root);
};

struct efi_time {
	uint16_t year;
	uint8_t month;
	uint8_t day;
	uint8_t hour;
	uint8_t minute;
	uint8_t second;
	uint8_t pad1;
	uint32_t nanosecond;
	int16_t time_zone;
	uint8_t daylight;
	uint8_t pad2;
};

struct efi_file_info {
	uint64_t size;
	uint64_t file_size;
	uint64_t physical_size;
	struct efi_time create_time;
	struct efi_time last_access_time;
	struct efi_time modification_time;
	uint64_t attribute;
	efi_char16_t file_name[1];
};

struct efi_pixel_bitmask {
	uint32_t red_mask;
	uint32_t green_mask;
	uint32_t blue_mask;
	uint32_t reserved_mask;
};

struct efi_graphics_output_mode_information {
	uint32_t version;
	uint32_t horizontal_resolution;
	uint32_t vertical_resolution;
	uint32_t pixel_format;
	struct efi_pixel_bitmask pixel_information;
	uint32_t pixels_per_scan_line;
};

struct efi_graphics_output_protocol_mode {
	uint32_t max_mode;
	uint32_t mode;
	struct efi_graphics_output_mode_information *info;
	efi_uintn_t size_of_info;
	efi_physical_address_t frame_buffer_base;
	efi_uintn_t frame_buffer_size;
};

struct efi_graphics_output_protocol {
	void *query_mode;
	void *set_mode;
	void *blt;
	struct efi_graphics_output_protocol_mode *mode;
};

static const struct efi_guid loaded_image_protocol_guid = {
	0x5b1b31a1,
	0x9562,
	0x11d2,
	{ 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b },
};

static const struct efi_guid simple_file_system_protocol_guid = {
	0x0964e5b22,
	0x6459,
	0x11d2,
	{ 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b },
};

static const struct efi_guid file_info_guid = {
	0x09576e92,
	0x6d3f,
	0x11d2,
	{ 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b },
};

static const struct efi_guid graphics_output_protocol_guid = {
	0x9042a9de,
	0x23dc,
	0x4a38,
	{ 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a },
};

static const struct efi_guid acpi_20_table_guid = {
	0x8868e871,
	0xe4f1,
	0x11d3,
	{ 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 },
};

static const struct efi_guid acpi_10_table_guid = {
	0xeb9d2d30,
	0x2d88,
	0x11d3,
	{ 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d },
};

static const efi_char16_t kernel_path[] = {
	'\\', 'E', 'F', 'I', '\\', 'O', 'S', '\\', 'K', 'E', 'R', 'N', 'E',
	'L', '.', 'E', 'L', 'F', 0,
};

__attribute__((used, section(".reloc"), aligned(4)))
static const uint32_t pe_reloc_directory[] = {
	0,
	8,
};

static void *mem_set(void *dest, int value, uint64_t count)
{
	uint8_t *out = dest;
	for (uint64_t i = 0; i < count; ++i) {
		out[i] = (uint8_t)value;
	}
	return dest;
}

static void *mem_copy(void *dest, const void *src, uint64_t count)
{
	uint8_t *out = dest;
	const uint8_t *in = src;
	for (uint64_t i = 0; i < count; ++i) {
		out[i] = in[i];
	}
	return dest;
}

static bool guid_equal(const struct efi_guid *a, const struct efi_guid *b)
{
	if (a->data1 != b->data1 || a->data2 != b->data2 ||
		a->data3 != b->data3) {
		return false;
	}
	for (uint64_t i = 0; i < sizeof(a->data4); ++i) {
		if (a->data4[i] != b->data4[i]) {
			return false;
		}
	}
	return true;
}

static void efi_puts(struct efi_system_table *st, const char *text)
{
	efi_char16_t buffer[128];
	efi_uintn_t used = 0;

	while (*text != '\0') {
		if (*text == '\n') {
			buffer[used++] = '\r';
			if (used == (sizeof(buffer) / sizeof(buffer[0])) - 1U) {
				buffer[used] = 0;
				(void)st->con_out->output_string(st->con_out, buffer);
				used = 0;
			}
		}

		buffer[used++] = (efi_char16_t)(uint8_t)*text++;
		if (used == (sizeof(buffer) / sizeof(buffer[0])) - 1U) {
			buffer[used] = 0;
			(void)st->con_out->output_string(st->con_out, buffer);
			used = 0;
		}
	}

	if (used != 0) {
		buffer[used] = 0;
		(void)st->con_out->output_string(st->con_out, buffer);
	}
}

static void die(struct efi_system_table *st, const char *message)
{
	efi_puts(st, "OS loader fatal: ");
	efi_puts(st, message);
	efi_puts(st, "\n");
	for (;;) {
		__asm__ volatile("hlt");
	}
}

static uint64_t align_down(uint64_t value, uint64_t alignment)
{
	return value & ~(alignment - 1ULL);
}

static uint64_t align_up(uint64_t value, uint64_t alignment)
{
	return (value + alignment - 1ULL) & ~(alignment - 1ULL);
}

/* Physical extent spanned by every alloc_low_pages() allocation.  The kernel
   keeps these pages live after ExitBootServices (page tables, boot_info, memory
   map), so the PMM must reserve them.  See struct os_boot_info. */
static uint64_t g_loader_lo = UINT64_MAX;
static uint64_t g_loader_hi;

static void *alloc_low_pages(struct efi_system_table *st, uint64_t pages)
{
	efi_physical_address_t address =
		OS_EARLY_DIRECT_MAP_SIZE - OS_PAGE_SIZE;
	efi_status_t status = st->boot_services->allocate_pages(
		EFI_ALLOCATE_MAX_ADDRESS, EFI_LOADER_DATA, pages, &address);
	if (EFI_ERROR(status)) {
		die(st, "AllocatePages below early direct-map limit failed");
	}
	mem_set((void *)(uintptr_t)address, 0, pages * OS_PAGE_SIZE);
	if (address < g_loader_lo) {
		g_loader_lo = address;
	}
	if (address + pages * OS_PAGE_SIZE > g_loader_hi) {
		g_loader_hi = address + pages * OS_PAGE_SIZE;
	}
	return (void *)(uintptr_t)address;
}

static uint64_t *alloc_page_table(struct efi_system_table *st)
{
	return alloc_low_pages(st, 1);
}

static uint64_t *page_table_next(struct efi_system_table *st, uint64_t *table,
	uint64_t index)
{
	if ((table[index] & PAGE_PRESENT) == 0) {
		uint64_t *next = alloc_page_table(st);
		table[index] = ((uint64_t)(uintptr_t)next & PAGE_MASK) |
			PAGE_PRESENT | PAGE_WRITABLE;
	}
	return (uint64_t *)(uintptr_t)(table[index] & PAGE_MASK);
}

static void map_4k_page(struct efi_system_table *st, uint64_t *pml4,
	uint64_t virt, uint64_t phys, uint64_t flags)
{
	uint64_t pml4i = (virt >> 39U) & 0x1ffU;
	uint64_t pdpti = (virt >> 30U) & 0x1ffU;
	uint64_t pdi = (virt >> 21U) & 0x1ffU;
	uint64_t pti = (virt >> 12U) & 0x1ffU;

	uint64_t *pdpt = page_table_next(st, pml4, pml4i);
	uint64_t *pd = page_table_next(st, pdpt, pdpti);
	uint64_t *pt = page_table_next(st, pd, pdi);
	pt[pti] = (phys & PAGE_MASK) | flags | PAGE_PRESENT;
}

static void map_4k_range(struct efi_system_table *st, uint64_t *pml4,
	uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags)
{
	uint64_t pages = align_up(size, OS_PAGE_SIZE) / OS_PAGE_SIZE;
	for (uint64_t i = 0; i < pages; ++i) {
		map_4k_page(st, pml4, virt + i * OS_PAGE_SIZE,
			phys + i * OS_PAGE_SIZE, flags);
	}
}

static void map_2m_page(struct efi_system_table *st, uint64_t *pml4,
	uint64_t virt, uint64_t phys, uint64_t flags)
{
	uint64_t pml4i = (virt >> 39U) & 0x1ffU;
	uint64_t pdpti = (virt >> 30U) & 0x1ffU;
	uint64_t pdi = (virt >> 21U) & 0x1ffU;

	uint64_t *pdpt = page_table_next(st, pml4, pml4i);
	uint64_t *pd = page_table_next(st, pdpt, pdpti);
	pd[pdi] = (phys & PAGE_MASK) | flags | PAGE_PRESENT | PAGE_LARGE;
}

static void map_2m_range(struct efi_system_table *st, uint64_t *pml4,
	uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags)
{
	uint64_t pages = align_up(size, TWO_MIB) / TWO_MIB;
	for (uint64_t i = 0; i < pages; ++i) {
		map_2m_page(st, pml4, virt + i * TWO_MIB, phys + i * TWO_MIB,
			flags);
	}
}

static struct efi_file_protocol *open_kernel_file(struct efi_system_table *st,
	efi_handle_t image_handle)
{
	struct efi_loaded_image_protocol *loaded_image = NULL;
	struct efi_simple_file_system_protocol *file_system = NULL;
	struct efi_file_protocol *root = NULL;
	struct efi_file_protocol *kernel = NULL;

	efi_status_t status = st->boot_services->handle_protocol(image_handle,
		&loaded_image_protocol_guid, (void **)&loaded_image);
	if (EFI_ERROR(status)) {
		die(st, "LoadedImage protocol unavailable");
	}

	status = st->boot_services->handle_protocol(loaded_image->device_handle,
		&simple_file_system_protocol_guid, (void **)&file_system);
	if (EFI_ERROR(status)) {
		die(st, "SimpleFileSystem protocol unavailable");
	}

	status = file_system->open_volume(file_system, &root);
	if (EFI_ERROR(status)) {
		die(st, "cannot open EFI volume");
	}

	status = root->open(root, &kernel, kernel_path, EFI_FILE_MODE_READ, 0);
	if (EFI_ERROR(status)) {
		die(st, "cannot open EFI\\OS\\KERNEL.ELF");
	}

	(void)root->close(root);
	return kernel;
}

static void *read_entire_kernel_file(struct efi_system_table *st,
	struct efi_file_protocol *kernel_file, uint64_t *file_size)
{
	efi_uintn_t info_size = 512;
	struct efi_file_info *info = NULL;
	efi_status_t status = st->boot_services->allocate_pool(EFI_LOADER_DATA,
		info_size, (void **)&info);
	if (EFI_ERROR(status)) {
		die(st, "cannot allocate file info buffer");
	}

	status = kernel_file->get_info(kernel_file, &file_info_guid, &info_size,
		info);
	if (status == EFI_BUFFER_TOO_SMALL) {
		(void)st->boot_services->free_pool(info);
		status = st->boot_services->allocate_pool(EFI_LOADER_DATA,
			info_size, (void **)&info);
		if (EFI_ERROR(status)) {
			die(st, "cannot allocate resized file info buffer");
		}
		status = kernel_file->get_info(kernel_file, &file_info_guid,
			&info_size, info);
	}
	if (EFI_ERROR(status)) {
		die(st, "cannot stat kernel ELF");
	}

	*file_size = info->file_size;
	void *buffer = NULL;
	status = st->boot_services->allocate_pool(EFI_LOADER_DATA,
		(efi_uintn_t)*file_size, &buffer);
	if (EFI_ERROR(status)) {
		die(st, "cannot allocate kernel file buffer");
	}

	status = kernel_file->set_position(kernel_file, 0);
	if (EFI_ERROR(status)) {
		die(st, "cannot seek kernel ELF");
	}

	efi_uintn_t read_size = (efi_uintn_t)*file_size;
	status = kernel_file->read(kernel_file, &read_size, buffer);
	if (EFI_ERROR(status) || read_size != (efi_uintn_t)*file_size) {
		die(st, "cannot read complete kernel ELF");
	}

	(void)st->boot_services->free_pool(info);
	return buffer;
}

static void validate_kernel_elf(struct efi_system_table *st,
	const struct elf64_ehdr *ehdr, uint64_t file_size)
{
	if (file_size < sizeof(*ehdr)) {
		die(st, "kernel ELF is too small");
	}
	if (ehdr->e_ident[0] != ELFMAG0 || ehdr->e_ident[1] != ELFMAG1 ||
		ehdr->e_ident[2] != ELFMAG2 || ehdr->e_ident[3] != ELFMAG3) {
		die(st, "kernel file is not ELF");
	}
	if (ehdr->e_ident[4] != ELFCLASS64 || ehdr->e_ident[5] != ELFDATA2LSB ||
		ehdr->e_ident[6] != EV_CURRENT) {
		die(st, "kernel ELF class/data/version invalid");
	}
	if (ehdr->e_type != ET_EXEC || ehdr->e_machine != EM_X86_64 ||
		ehdr->e_version != EV_CURRENT) {
		die(st, "kernel ELF target invalid");
	}
	if (ehdr->e_phentsize != sizeof(struct elf64_phdr) ||
		ehdr->e_phnum == 0) {
		die(st, "kernel ELF program headers invalid");
	}
	uint64_t ph_end = ehdr->e_phoff +
		(uint64_t)ehdr->e_phnum * sizeof(struct elf64_phdr);
	if (ehdr->e_phoff >= file_size || ph_end > file_size) {
		die(st, "kernel ELF program headers out of range");
	}
	if (ehdr->e_entry < OS_KERNEL_VMA_BASE) {
		die(st, "kernel entry is not higher-half");
	}
}

static void load_kernel_segments(struct efi_system_table *st,
	const uint8_t *kernel_file, uint64_t file_size,
	const struct elf64_ehdr *ehdr, struct os_boot_info *boot_info,
	uint64_t *pml4)
{
	const struct elf64_phdr *phdrs =
		(const struct elf64_phdr *)(kernel_file + ehdr->e_phoff);
	uint64_t phys_start = UINT64_MAX;
	uint64_t phys_end = 0;
	uint64_t virt_start = UINT64_MAX;
	uint64_t virt_end = 0;

	for (uint16_t i = 0; i < ehdr->e_phnum; ++i) {
		const struct elf64_phdr *ph = &phdrs[i];
		if (ph->p_type != PT_LOAD) {
			continue;
		}
		if (ph->p_memsz < ph->p_filesz ||
			ph->p_offset + ph->p_filesz > file_size) {
			die(st, "kernel PT_LOAD range invalid");
		}
		if (ph->p_vaddr < OS_KERNEL_VMA_BASE || ph->p_paddr == 0) {
			die(st, "kernel PT_LOAD address invalid");
		}

		uint64_t load_start = align_down(ph->p_paddr, OS_PAGE_SIZE);
		uint64_t load_end = align_up(ph->p_paddr +
			ph->p_memsz, OS_PAGE_SIZE);
		efi_physical_address_t alloc_addr = load_start;
		efi_uintn_t page_count = (load_end - load_start) / OS_PAGE_SIZE;
		efi_status_t status = st->boot_services->allocate_pages(
			EFI_ALLOCATE_ADDRESS, EFI_LOADER_DATA, page_count,
			&alloc_addr);
		if (EFI_ERROR(status)) {
			die(st, "cannot allocate kernel segment at requested paddr");
		}

		mem_set((void *)(uintptr_t)load_start, 0, load_end - load_start);
		mem_copy((void *)(uintptr_t)ph->p_paddr,
			kernel_file + ph->p_offset, ph->p_filesz);

		uint64_t map_flags = PAGE_PRESENT;
		if ((ph->p_flags & PF_W) != 0) {
			map_flags |= PAGE_WRITABLE;
		}
		map_4k_range(st, pml4, ph->p_vaddr, ph->p_paddr,
			ph->p_memsz, map_flags);

		if (load_start < phys_start) {
			phys_start = load_start;
		}
		if (load_end > phys_end) {
			phys_end = load_end;
		}
		if (ph->p_vaddr < virt_start) {
			virt_start = ph->p_vaddr;
		}
		if (ph->p_vaddr + ph->p_memsz > virt_end) {
			virt_end = ph->p_vaddr + ph->p_memsz;
		}
	}

	if (phys_start == UINT64_MAX) {
		die(st, "kernel ELF contains no loadable segments");
	}

	boot_info->kernel_phys_start = phys_start;
	boot_info->kernel_phys_end = phys_end;
	boot_info->kernel_virt_start = virt_start;
	boot_info->kernel_virt_end = align_up(virt_end, OS_PAGE_SIZE);
}

static void populate_framebuffer(struct efi_system_table *st,
	struct os_boot_info *boot_info)
{
	struct efi_graphics_output_protocol *gop = NULL;
	efi_status_t status = st->boot_services->locate_protocol(
		&graphics_output_protocol_guid, NULL, (void **)&gop);
	if (EFI_ERROR(status) || gop == NULL || gop->mode == NULL ||
		gop->mode->info == NULL) {
		return;
	}

	const struct efi_graphics_output_mode_information *info =
		gop->mode->info;
	boot_info->framebuffer.base = gop->mode->frame_buffer_base;
	boot_info->framebuffer.width = info->horizontal_resolution;
	boot_info->framebuffer.height = info->vertical_resolution;
	boot_info->framebuffer.pixels_per_scanline = info->pixels_per_scan_line;
	boot_info->framebuffer.red_mask = info->pixel_information.red_mask;
	boot_info->framebuffer.green_mask = info->pixel_information.green_mask;
	boot_info->framebuffer.blue_mask = info->pixel_information.blue_mask;
	boot_info->framebuffer.reserved_mask =
		info->pixel_information.reserved_mask;

	switch (info->pixel_format) {
	case 0:
		boot_info->framebuffer.format = OS_FB_FORMAT_RGB;
		break;
	case 1:
		boot_info->framebuffer.format = OS_FB_FORMAT_BGR;
		break;
	case 2:
		boot_info->framebuffer.format = OS_FB_FORMAT_BITMASK;
		break;
	default:
		boot_info->framebuffer.format = OS_FB_FORMAT_UNKNOWN;
		break;
	}
}

static uint64_t find_rsdp(struct efi_system_table *st)
{
	for (efi_uintn_t i = 0; i < st->number_of_table_entries; ++i) {
		const struct efi_configuration_table *table =
			&st->configuration_table[i];
		if (guid_equal(&table->vendor_guid, &acpi_20_table_guid) ||
			guid_equal(&table->vendor_guid, &acpi_10_table_guid)) {
			return (uint64_t)(uintptr_t)table->vendor_table;
		}
	}
	return 0;
}

static void get_final_memory_map_and_exit(struct efi_system_table *st,
	efi_handle_t image_handle, struct os_boot_info *boot_info)
{
	efi_uintn_t map_size = 0;
	efi_uintn_t map_key = 0;
	efi_uintn_t descriptor_size = 0;
	uint32_t descriptor_version = 0;
	efi_status_t status = st->boot_services->get_memory_map(&map_size, NULL,
		&map_key, &descriptor_size, &descriptor_version);
	if (status != EFI_BUFFER_TOO_SMALL || descriptor_size == 0) {
		die(st, "cannot size UEFI memory map");
	}

	map_size += descriptor_size * 32U;
	void *memory_map = alloc_low_pages(st, align_up(map_size, OS_PAGE_SIZE) /
		OS_PAGE_SIZE);

	for (;;) {
		efi_uintn_t current_size = map_size;
		status = st->boot_services->get_memory_map(&current_size, memory_map,
			&map_key, &descriptor_size, &descriptor_version);
		if (status == EFI_BUFFER_TOO_SMALL) {
			map_size += descriptor_size * 32U;
			memory_map = alloc_low_pages(st, align_up(map_size,
				OS_PAGE_SIZE) / OS_PAGE_SIZE);
			continue;
		}
		if (EFI_ERROR(status)) {
			die(st, "cannot read UEFI memory map");
		}

		boot_info->memory_map = (uint64_t)(uintptr_t)memory_map;
		boot_info->memory_map_size = current_size;
		boot_info->memory_descriptor_size = descriptor_size;
		boot_info->memory_descriptor_version = descriptor_version;

		/* Capture the loader allocation extent last, after the memory map
		   (the final alloc_low_pages caller) so the PMM reserves it all. */
		boot_info->loader_reserved_start = g_loader_lo;
		boot_info->loader_reserved_end = g_loader_hi;

		status = st->boot_services->exit_boot_services(image_handle,
			map_key);
		if (status == EFI_SUCCESS) {
			return;
		}
		if (EFI_ERROR(status)) {
			continue;
		}
	}
}

static void switch_page_table(uint64_t pml4_phys)
{
	__asm__ volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
}

typedef void (*kernel_entry_t)(const struct os_boot_info *boot_info);

efi_status_t EFIAPI efi_main(efi_handle_t image_handle,
	struct efi_system_table *system_table)
{
	struct efi_system_table *st = system_table;
	(void)st->boot_services->set_watchdog_timer(0, 0, 0, NULL);
	efi_puts(st, "OS loader: starting\n");

	struct efi_file_protocol *kernel_file = open_kernel_file(st,
		image_handle);
	uint64_t kernel_file_size = 0;
	uint8_t *kernel_buffer = read_entire_kernel_file(st, kernel_file,
		&kernel_file_size);
	(void)kernel_file->close(kernel_file);

	const struct elf64_ehdr *ehdr = (const struct elf64_ehdr *)kernel_buffer;
	validate_kernel_elf(st, ehdr, kernel_file_size);

	struct os_boot_info *boot_info = alloc_low_pages(st, 1);
	boot_info->magic = OS_BOOTINFO_MAGIC;
	boot_info->version = OS_BOOTINFO_VERSION;
	boot_info->size = sizeof(*boot_info);
	boot_info->direct_map_base = OS_DIRECT_MAP_BASE;
	boot_info->direct_map_size = OS_EARLY_DIRECT_MAP_SIZE;
	boot_info->rsdp = find_rsdp(st);
	populate_framebuffer(st, boot_info);

	uint64_t *pml4 = alloc_page_table(st);
	map_2m_range(st, pml4, 0, 0, OS_EARLY_DIRECT_MAP_SIZE,
		PAGE_WRITABLE);
	map_2m_range(st, pml4, OS_DIRECT_MAP_BASE, 0,
		OS_EARLY_DIRECT_MAP_SIZE, PAGE_WRITABLE);

	load_kernel_segments(st, kernel_buffer, kernel_file_size, ehdr,
		boot_info, pml4);
	boot_info->page_table_root = (uint64_t)(uintptr_t)pml4;

	efi_puts(st, "OS loader: exiting boot services\n");
	get_final_memory_map_and_exit(st, image_handle, boot_info);
	switch_page_table((uint64_t)(uintptr_t)pml4);

	kernel_entry_t entry = (kernel_entry_t)(uintptr_t)ehdr->e_entry;
	entry(boot_info);

	for (;;) {
		__asm__ volatile("hlt");
	}
}
