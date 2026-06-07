#ifndef KERNEL_ARCH_X86_64_SMP_H
#define KERNEL_ARCH_X86_64_SMP_H

#include "acpi/acpi.h"

/* Start all APs found in the MADT. Returns the number of APs started. */
uint32_t smp_init(const struct acpi_madt_info *madt);

/* AP C entry point — called from the trampoline after entering 64-bit mode. */
void ap_entry(void);

#endif
