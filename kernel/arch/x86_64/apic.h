#ifndef KERNEL_ARCH_X86_64_APIC_H
#define KERNEL_ARCH_X86_64_APIC_H

#include <stdint.h>

/* Vector assigned to the periodic APIC timer interrupt. */
#define APIC_TIMER_VECTOR 0x20

void lapic_init(uint64_t lapic_phys, uint64_t direct_map_base);
void lapic_ap_init(void);     /* called by each AP to enable its own LAPIC */
void lapic_eoi(void);
uint32_t lapic_id(void);

/* Calibrate and arm the APIC timer in periodic mode.
   Returns the measured frequency in Hz. */
uint64_t apic_timer_init(void);

/* Send an IPI to the target LAPIC ID. */
void lapic_send_ipi(uint8_t target_lapic_id, uint32_t icr_lo);

#endif
