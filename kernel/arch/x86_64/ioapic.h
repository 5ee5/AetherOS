#ifndef KERNEL_ARCH_X86_64_IOAPIC_H
#define KERNEL_ARCH_X86_64_IOAPIC_H

#include <stdbool.h>
#include <stdint.h>

void ioapic_init(uint64_t ioapic_phys, uint8_t ioapic_id,
                 uint32_t gsi_base, uint64_t direct_map_base);

/* Route a GSI to a CPU vector.
   active_low: true for active-low polarity (level-triggered).
   level:      true for level-triggered, false for edge-triggered. */
void ioapic_route(uint32_t gsi, uint8_t vector, uint8_t lapic_id,
                  bool active_low, bool level);

/* Mask/unmask a GSI. */
void ioapic_mask(uint32_t gsi);
void ioapic_unmask(uint32_t gsi);

#endif
