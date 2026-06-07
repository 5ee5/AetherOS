#include "arch/x86_64/apic.h"

#include <stdint.h>

#include "arch/x86_64/io.h"
#include "core/panic.h"
#include "core/serial.h"
#include "mem/vmm.h"

/* ---- LAPIC register offsets (byte offsets from LAPIC base) ------------ */
#define LAPIC_ID            0x020U
#define LAPIC_VERSION       0x030U
#define LAPIC_TPR           0x080U
#define LAPIC_EOI           0x0b0U
#define LAPIC_ICR_LO        0x300U
#define LAPIC_ICR_HI        0x310U
#define LAPIC_SPURIOUS      0x0f0U
#define LAPIC_LVT_TIMER     0x320U
#define LAPIC_TIMER_INITCNT 0x380U
#define LAPIC_TIMER_CURCNT  0x390U
#define LAPIC_TIMER_DCR     0x3e0U

/* Spurious vector register bits. */
#define LAPIC_SPURIOUS_ENABLE 0x100U
#define LAPIC_SPURIOUS_VECTOR 0xffU

/* Timer LVT bits. */
#define LAPIC_TIMER_PERIODIC  (1U << 17U)
#define LAPIC_TIMER_MASKED    (1U << 16U)

/* Divide configuration: divide by 16. */
#define LAPIC_DCR_DIV16 0x3U

/* ---- PIT constants for calibration ------------------------------------ */
#define PIT_CHANNEL0  0x40U
#define PIT_CMD       0x43U
#define PIT_FREQ      1193182ULL   /* Hz */
#define CAL_MS        10U          /* calibration window in milliseconds */
#define CAL_TICKS     ((uint16_t)((PIT_FREQ * CAL_MS) / 1000U))

/* ---- Module state ----------------------------------------------------- */
#define LAPIC_VIRT_BASE 0xffffa00000000000ULL

static volatile uint32_t *lapic_regs;

/* ---- Register helpers ------------------------------------------------- */

static uint32_t lapic_read(uint32_t reg)
{
	return lapic_regs[reg / 4U];
}

static void lapic_write(uint32_t reg, uint32_t val)
{
	lapic_regs[reg / 4U] = val;
}

/* ---- 8259 PIC --------------------------------------------------------- */

static void pic_disable(void)
{
	/* Reinitialize both PICs then mask every IRQ. */
	outb(0x20, 0x11);   /* ICW1: init, edge, cascade */
	outb(0xa0, 0x11);
	outb(0x21, 0x20);   /* ICW2: remap master to vector 0x20 */
	outb(0xa1, 0x28);   /* ICW2: remap slave  to vector 0x28 */
	outb(0x21, 0x04);   /* ICW3: slave on IRQ2 */
	outb(0xa1, 0x02);
	outb(0x21, 0x01);   /* ICW4: 8086 mode */
	outb(0xa1, 0x01);
	outb(0x21, 0xff);   /* OCW1: mask all master IRQs */
	outb(0xa1, 0xff);   /* OCW1: mask all slave  IRQs */
}

/* ---- PIT one-shot calibration ----------------------------------------- */

static void pit_start_oneshot(uint16_t ticks)
{
	outb(PIT_CMD, 0x30);                       /* channel 0, mode 0, binary */
	outb(PIT_CHANNEL0, (uint8_t)(ticks & 0xffU));
	outb(PIT_CHANNEL0, (uint8_t)(ticks >> 8U));
}

static void pit_wait_done(void)
{
	/* Poll channel 0 status until OUT goes high (count expired). */
	do {
		outb(PIT_CMD, 0xe2);   /* read-back: latch status of channel 0 */
	} while ((inb(PIT_CHANNEL0) & 0x80U) == 0U);
}

/* ---- Public API ------------------------------------------------------- */

void lapic_init(uint64_t lapic_phys, uint64_t direct_map_base)
{
	(void)direct_map_base;

	/* Map the LAPIC's 4 KiB MMIO page into a fixed virtual address. */
	if (!vmm_map(LAPIC_VIRT_BASE, lapic_phys, VMM_WRITABLE)) {
		panic("lapic: failed to map MMIO page");
	}
	lapic_regs = (volatile uint32_t *)(uintptr_t)LAPIC_VIRT_BASE;

	pic_disable();

	/* Enable the LAPIC: set spurious vector and software-enable bit. */
	lapic_write(LAPIC_SPURIOUS,
		LAPIC_SPURIOUS_ENABLE | (LAPIC_SPURIOUS_VECTOR & 0xffU) | 0xf0U);

	/* Accept all priority interrupts. */
	lapic_write(LAPIC_TPR, 0);

	serial_write("lapic: id=");
	serial_write_dec(lapic_read(LAPIC_ID) >> 24U);
	serial_write(" version=");
	serial_write_hex(lapic_read(LAPIC_VERSION));
	serial_write("\n");
}

uint64_t apic_timer_init(void)
{
	/* Mask timer and set divide-by-16. */
	lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_MASKED | APIC_TIMER_VECTOR);
	lapic_write(LAPIC_TIMER_DCR, LAPIC_DCR_DIV16);

	/* Start the PIT one-shot and APIC timer simultaneously. */
	lapic_write(LAPIC_TIMER_INITCNT, 0xffffffffU);
	pit_start_oneshot(CAL_TICKS);
	pit_wait_done();

	uint32_t remaining = lapic_read(LAPIC_TIMER_CURCNT);
	uint32_t ticks_elapsed = 0xffffffffU - remaining;

	/* ticks_elapsed counts in units of (bus_freq / 16).
	   bus_freq = ticks_elapsed * 16 * (1000 / CAL_MS) */
	uint64_t bus_freq = (uint64_t)ticks_elapsed * 16ULL * (1000ULL / CAL_MS);

	serial_write("lapic: timer bus_freq=");
	serial_write_dec(bus_freq / 1000000ULL);
	serial_write(" MHz\n");

	/* Arm the timer in periodic mode targeting ~1000 Hz. */
	uint32_t period = (uint32_t)(bus_freq / (16ULL * 1000ULL));
	if (period == 0) {
		period = 1;
	}
	lapic_write(LAPIC_TIMER_INITCNT, period);
	lapic_write(LAPIC_LVT_TIMER,
		LAPIC_TIMER_PERIODIC | APIC_TIMER_VECTOR);

	serial_write("lapic: timer armed, period=");
	serial_write_dec(period);
	serial_write(" (~1000 Hz)\n");

	return bus_freq;
}

void lapic_ap_init(void)
{
	lapic_write(LAPIC_SPURIOUS,
		LAPIC_SPURIOUS_ENABLE | (LAPIC_SPURIOUS_VECTOR & 0xffU) | 0xf0U);
	lapic_write(LAPIC_TPR, 0);

	/* Arm timer using the same period the BSP measured. */
	uint32_t period = lapic_read(LAPIC_TIMER_INITCNT);
	lapic_write(LAPIC_TIMER_DCR, LAPIC_DCR_DIV16);
	lapic_write(LAPIC_TIMER_INITCNT, period);
	lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_PERIODIC | APIC_TIMER_VECTOR);
}

void lapic_eoi(void)
{
	lapic_write(LAPIC_EOI, 0);
}

uint32_t lapic_id(void)
{
	return lapic_read(LAPIC_ID) >> 24U;
}

void lapic_send_ipi(uint8_t target_lapic_id, uint32_t icr_lo)
{
	/* Write destination first (high word), then command (low word). */
	lapic_write(LAPIC_ICR_HI, (uint32_t)target_lapic_id << 24U);
	lapic_write(LAPIC_ICR_LO, icr_lo);
	/* Spin until the IPI is accepted (delivery status bit clears). */
	while (lapic_read(LAPIC_ICR_LO) & (1U << 12U)) {
		__asm__ volatile("pause");
	}
}
