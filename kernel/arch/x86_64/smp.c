#include "arch/x86_64/smp.h"

#include <stddef.h>
#include <stdint.h>

#include "acpi/acpi.h"
#include "arch/x86_64/apic.h"
#include "arch/x86_64/cpu.h"
#include "core/panic.h"
#include "core/serial.h"
#include "mem/heap.h"
#include "mem/vmm.h"
#include "sched/sched.h"

/* Physical address of the AP startup trampoline. */
#define TRAMPOLINE_PHYS 0x8000ULL

/* Offsets within the trampoline (from its physical base). */
#define TRAM_PML4  0x0F00ULL
#define TRAM_ENTRY 0x0F08ULL
#define TRAM_STACK 0x0F10ULL
#define TRAM_READY 0x0F18ULL

/* SIPI vector: SIPI vector byte = TRAMPOLINE_PHYS >> 12 = 0x08 */
#define SIPI_VECTOR ((uint32_t)(TRAMPOLINE_PHYS >> 12U))

/* ICR delivery modes. */
#define IPI_INIT  0x00004500U   /* INIT, level-assert, edge */
#define IPI_SIPI  0x00004600U   /* Start-up, edge */
#define IPI_DEASSERT_INIT 0x00008500U  /* INIT de-assert */

/* AP stack size. */
#define AP_STACK_SIZE 65536ULL

extern const uint8_t smp_trampoline_start[];
extern const uint8_t smp_trampoline_end[];

/* Access physical memory through the direct map. */
static volatile uint64_t *dm_qword(uint64_t phys)
{
	return (volatile uint64_t *)(uintptr_t)(vmm_direct_map_base() + phys);
}

static volatile uint32_t *dm_dword(uint64_t phys)
{
	return (volatile uint32_t *)(uintptr_t)(vmm_direct_map_base() + phys);
}

static void spin_delay(uint64_t iters)
{
	for (volatile uint64_t i = 0; i < iters; ++i) {
		__asm__ volatile("pause");
	}
}

static void copy_trampoline(void)
{
	volatile uint8_t *dst =
		(volatile uint8_t *)(uintptr_t)(vmm_direct_map_base() + TRAMPOLINE_PHYS);
	const uint8_t *src = smp_trampoline_start;
	uint64_t len = (uint64_t)(smp_trampoline_end - smp_trampoline_start);
	for (uint64_t i = 0; i < len; ++i) {
		dst[i] = src[i];
	}
}

static bool start_ap(uint8_t target_id, uint64_t entry, uint64_t stack_top)
{
	/* Fill boot data. */
	*dm_qword(TRAMPOLINE_PHYS + TRAM_PML4)  = vmm_pml4_phys();
	*dm_qword(TRAMPOLINE_PHYS + TRAM_ENTRY) = entry;
	*dm_qword(TRAMPOLINE_PHYS + TRAM_STACK) = stack_top;
	*dm_dword(TRAMPOLINE_PHYS + TRAM_READY) = 0;

	/* INIT IPI. */
	lapic_send_ipi(target_id, IPI_INIT);
	spin_delay(100000ULL);   /* ~10 ms */

	/* First SIPI. */
	lapic_send_ipi(target_id, IPI_SIPI | SIPI_VECTOR);
	spin_delay(2000ULL);     /* ~200 µs */

	if (*dm_dword(TRAMPOLINE_PHYS + TRAM_READY) != 0U) {
		return true;
	}

	/* Second SIPI (retry per Intel spec). */
	lapic_send_ipi(target_id, IPI_SIPI | SIPI_VECTOR);
	spin_delay(2000ULL);

	/* Wait up to ~1s for the AP to signal. */
	for (uint64_t t = 0; t < 1000000ULL; ++t) {
		if (*dm_dword(TRAMPOLINE_PHYS + TRAM_READY) != 0U) {
			return true;
		}
		__asm__ volatile("pause");
	}
	return false;
}

uint32_t smp_init(const struct acpi_madt_info *madt)
{
	if (madt->cpu_count <= 1U) {
		serial_write("smp: only one CPU in MADT, skipping AP startup\n");
		return 0;
	}

	copy_trampoline();

	uint8_t bsp_id  = (uint8_t)lapic_id();
	uint32_t started = 0;

	for (uint32_t i = 0; i < madt->cpu_count; ++i) {
		uint8_t id = madt->cpu_lapic_ids[i];
		if (id == bsp_id) {
			continue;
		}
		if (id >= MAX_CPUS) {
			/* Per-CPU tables are indexed by LAPIC ID and hold only
			   MAX_CPUS slots; we cannot track this AP, so skip it
			   rather than overrun those arrays. */
			serial_write("smp: skipping AP, lapic_id >= MAX_CPUS: ");
			serial_write_dec(id);
			serial_write("\n");
			continue;
		}

		void *stack = kmalloc(AP_STACK_SIZE);
		if (stack == NULL) {
			panic("smp: AP stack alloc failed");
		}
		uint64_t stack_top = (uint64_t)(uintptr_t)stack + AP_STACK_SIZE;

		/* Pre-allocate idle thread from BSP so ap_entry() does no allocs. */
		sched_ap_prepare((uint32_t)id);

		serial_write("smp: starting AP lapic_id=");
		serial_write_dec(id);
		serial_write("\n");

		if (start_ap(id, (uint64_t)(uintptr_t)ap_entry, stack_top)) {
			++started;
			serial_write("smp: AP online lapic_id=");
			serial_write_dec(id);
			serial_write("\n");
		} else {
			serial_write("smp: AP timeout lapic_id=");
			serial_write_dec(id);
			serial_write("\n");
		}
	}

	return started;
}

void ap_entry(void)
{
	lapic_ap_init();
	sched_ap_enter();
	/* Enter the idle loop — the timer ISR will context-switch away. */
	sched_idle_loop();
	/* Unreachable. */
	for (;;) {
		__asm__ volatile("hlt");
	}
}