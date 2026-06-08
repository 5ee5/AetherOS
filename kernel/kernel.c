#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "acpi/acpi.h"
#include "arch/x86_64/apic.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/ioapic.h"
#include "arch/x86_64/smp.h"
#include "arch/x86_64/tss.h"
#include "arch/x86_64/io.h"
#include "drivers/ahci.h"
#include "drivers/ps2kbd.h"
#include "fs/vfs.h"
#include "gui/desktop.h"
#include "gui/fb_draw.h"
#include "net/net.h"
#include "proc/process.h"
#include "sched/sched.h"
#include "sched/thread.h"
#include "syscall/syscall.h"
#include "core/fb.h"
#include "core/panic.h"
#include "core/serial.h"
#include "mem/heap.h"
#include "mem/pmm.h"
#include "mem/vmm.h"
#include "os/bootinfo.h"
#include "test/tests.h"

extern char __kernel_virt_start[];
extern char __kernel_virt_end[];
extern char __kernel_phys_start[];
extern char __kernel_phys_end[];
extern char kernel_stack_guard[];

void kernel_main(const struct os_boot_info *boot_info);
static void kernel_gui_thread(void *arg);

static void validate_boot_info(const struct os_boot_info *boot_info)
{
	if (boot_info == 0) {
		panic("missing boot info");
	}
	if (boot_info->magic != OS_BOOTINFO_MAGIC) {
		panic("invalid boot info magic");
	}
	if (boot_info->version != OS_BOOTINFO_VERSION ||
		boot_info->size < sizeof(*boot_info)) {
		panic("unsupported boot info version");
	}
	if (boot_info->memory_map == 0 || boot_info->memory_map_size == 0 ||
		boot_info->memory_descriptor_size <
			sizeof(struct os_uefi_memory_descriptor)) {
		panic("invalid UEFI memory map");
	}
	if (boot_info->direct_map_base != OS_DIRECT_MAP_BASE ||
		boot_info->direct_map_size < OS_EARLY_DIRECT_MAP_SIZE) {
		panic("invalid direct map");
	}
}

static void kernel_gui_thread(void *arg)
{
	(void)arg;
	desktop_init();
	for (;;) { desktop_tick(); thread_yield(); }
}

static void print_boot_summary(const struct os_boot_info *boot_info)
{
	serial_write("kernel: higher-half entry reached\n");
	serial_write("kernel: virt=[");
	serial_write_hex((uint64_t)(uintptr_t)__kernel_virt_start);
	serial_write(", ");
	serial_write_hex((uint64_t)(uintptr_t)__kernel_virt_end);
	serial_write(") phys=[");
	serial_write_hex((uint64_t)(uintptr_t)__kernel_phys_start);
	serial_write(", ");
	serial_write_hex((uint64_t)(uintptr_t)__kernel_phys_end);
	serial_write(")\n");

	serial_write("kernel: loader phys=[");
	serial_write_hex(boot_info->kernel_phys_start);
	serial_write(", ");
	serial_write_hex(boot_info->kernel_phys_end);
	serial_write(") rsdp=");
	serial_write_hex(boot_info->rsdp);
	serial_write("\n");

	if (boot_info->framebuffer.base != 0) {
		serial_write("kernel: framebuffer ");
		serial_write_dec(boot_info->framebuffer.width);
		serial_write("x");
		serial_write_dec(boot_info->framebuffer.height);
		serial_write(" stride=");
		serial_write_dec(boot_info->framebuffer.pixels_per_scanline);
		serial_write(" base=");
		serial_write_hex(boot_info->framebuffer.base);
		serial_write("\n");
	}
}

static void run_early_self_checks(void)
{
	uint64_t translated = 0;
	if (!vmm_virt_to_phys((uint64_t)(uintptr_t)&kernel_main, &translated)) {
		panic("VMM cannot translate kernel_main");
	}

	uint64_t page = pmm_alloc_page();
	if (page == PMM_ALLOC_FAILED || !pmm_address_in_range(page)) {
		panic("PMM allocation failed");
	}
	pmm_free_page(page);

	serial_write("selftest: kernel_main phys=");
	serial_write_hex(translated);
	serial_write(" pmm_alloc=");
	serial_write_hex(page);
	serial_write("\n");
}

void kernel_main(const struct os_boot_info *boot_info)
{
	serial_init();
	serial_write("\n");
	serial_write("kernel: serial online\n");

	validate_boot_info(boot_info);
	print_boot_summary(boot_info);

	x86_64_gdt_init();
	serial_write("kernel: gdt installed\n");

	tss_init();
	serial_write("kernel: tss installed\n");

	syscall_init();

	x86_64_idt_init();
	serial_write("kernel: idt installed\n");

	fb_init(&boot_info->framebuffer, boot_info->direct_map_base);
	fb_clear(0x00101820U);

	pmm_init(boot_info);
	vmm_init(boot_info);
	if (!vmm_unmap((uint64_t)(uintptr_t)kernel_stack_guard)) {
		panic("failed to install stack guard page");
	}
	serial_write("kernel: stack guard page installed\n");
	heap_init();

	acpi_init(boot_info->rsdp, boot_info->direct_map_base);
	const struct acpi_madt_info *madt = acpi_madt();
	if (madt == NULL) {
		panic("ACPI MADT not found");
	}

	lapic_init(madt->local_apic_base, boot_info->direct_map_base);
	apic_timer_init();
	serial_write("kernel: lapic and timer online\n");

	if (madt->ioapic_found) {
		ioapic_init(madt->ioapic_base, madt->ioapic_id,
			madt->ioapic_gsi_base, boot_info->direct_map_base);
		serial_write("kernel: ioapic online\n");
	}

	sched_init();
	serial_write("kernel: scheduler online\n");

	smp_init(madt);
	serial_write("kernel: smp init done\n");

	run_early_self_checks();

	ahci_init();
	vfs_init();
	ps2kbd_init();

	fb_draw_init();

	net_init();

	ktest_run_all();

	/* Try loading /bin/login from ext2; fall back to the embedded shell blob. */
	{
		extern const uint8_t shell_elf_start[], shell_elf_end[];
		bool launched = false;
		int lfd = vfs_open("/bin/login");
		if (lfd >= 0) {
			uint64_t lsz = vfs_file_size("/bin/login");
			if (lsz > 0 && lsz < 4U * 1024U * 1024U) {
				void *lbuf = kmalloc((uint32_t)lsz);
				if (lbuf) {
					vfs_read(lfd, lbuf, (uint32_t)lsz);
					vfs_close(lfd);
					launched = (process_create_from_elf(lbuf, lsz) != NULL);
					kfree(lbuf);
				} else {
					vfs_close(lfd);
				}
			} else {
				vfs_close(lfd);
			}
		}
		if (!launched) {
			serial_write("kernel: /bin/login not found, using embedded shell\n");
			process_create_from_elf(shell_elf_start,
			    (uint64_t)(shell_elf_end - shell_elf_start));
		}
	}

	/* Start the GUI desktop thread. */
	thread_create(kernel_gui_thread, NULL);

	serial_write("kernel: initialization complete\n");

	/* Enable interrupts so the scheduler can run the user process,
	   then idle. */
	for (;;) {
		__asm__ volatile("sti; hlt" ::: "memory");
	}
}
