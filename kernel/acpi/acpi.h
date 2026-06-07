#ifndef KERNEL_ACPI_ACPI_H
#define KERNEL_ACPI_ACPI_H

#include <stdbool.h>
#include <stdint.h>

#define ACPI_MAX_CPUS 64

/* Parsed MADT data needed by interrupt subsystem. */
struct acpi_madt_info {
	uint64_t local_apic_base;   /* physical base from MADT header */
	uint64_t ioapic_base;       /* physical base of first I/O APIC */
	uint8_t  ioapic_id;
	uint32_t ioapic_gsi_base;   /* global system interrupt base */
	bool     ioapic_found;

	/* All enabled local APIC IDs, index 0 = BSP (first in MADT). */
	uint8_t  cpu_lapic_ids[ACPI_MAX_CPUS];
	uint32_t cpu_count;
};

void acpi_init(uint64_t rsdp_phys, uint64_t direct_map_base);
const struct acpi_madt_info *acpi_madt(void);

#endif
