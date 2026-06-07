#include "arch/x86_64/ioapic.h"

#include <stdint.h>

#include "core/panic.h"
#include "core/serial.h"
#include "mem/vmm.h"

/* ---- IOAPIC MMIO layout ----------------------------------------------- */

/* Two 32-bit registers at the IOAPIC base. */
#define IOREGSEL_OFF  0x00U   /* index register */
#define IOWIN_OFF     0x10U   /* data window    */

/* IOAPIC indirect registers. */
#define IOAPIC_ID_REG     0x00U
#define IOAPIC_VER_REG    0x01U
#define IOAPIC_REDIR_BASE 0x10U   /* 0x10 + 2*n = low word of entry n */

/* Redirection entry bits. */
#define REDIR_MASKED      (1U << 16U)
#define REDIR_LEVEL       (1U << 15U)
#define REDIR_ACTIVE_LOW  (1U << 13U)
#define REDIR_LOGICAL     (1U << 11U)

/* ---- Module state ----------------------------------------------------- */

#define IOAPIC_VIRT_BASE 0xffffa00000001000ULL  /* one page after LAPIC */

static volatile uint32_t *ioapic_mmio;
static uint32_t gsi_base_g;
static uint32_t max_redir;

/* ---- Register helpers ------------------------------------------------- */

static void ioapic_write(uint8_t reg, uint32_t val)
{
	ioapic_mmio[IOREGSEL_OFF / 4U] = reg;
	ioapic_mmio[IOWIN_OFF / 4U]    = val;
}

static uint32_t ioapic_read(uint8_t reg)
{
	ioapic_mmio[IOREGSEL_OFF / 4U] = reg;
	return ioapic_mmio[IOWIN_OFF / 4U];
}

static void write_redir(uint32_t index, uint32_t lo, uint32_t hi)
{
	ioapic_write((uint8_t)(IOAPIC_REDIR_BASE + index * 2U),     lo);
	ioapic_write((uint8_t)(IOAPIC_REDIR_BASE + index * 2U + 1U), hi);
}

static uint32_t read_redir_lo(uint32_t index)
{
	return ioapic_read((uint8_t)(IOAPIC_REDIR_BASE + index * 2U));
}

/* ---- Public API ------------------------------------------------------- */

void ioapic_init(uint64_t ioapic_phys, uint8_t ioapic_id,
                 uint32_t gsi_base, uint64_t direct_map_base)
{
	(void)ioapic_id;
	(void)direct_map_base;

	if (!vmm_map(IOAPIC_VIRT_BASE, ioapic_phys, VMM_WRITABLE)) {
		panic("ioapic: failed to map MMIO page");
	}
	ioapic_mmio = (volatile uint32_t *)(uintptr_t)IOAPIC_VIRT_BASE;
	gsi_base_g  = gsi_base;

	uint32_t ver = ioapic_read(IOAPIC_VER_REG);
	max_redir = (ver >> 16U) & 0xffU;

	/* Mask all redirection entries. */
	for (uint32_t i = 0; i <= max_redir; ++i) {
		write_redir(i, REDIR_MASKED, 0);
	}

	serial_write("ioapic: phys=");
	serial_write_hex(ioapic_phys);
	serial_write(" gsi_base=");
	serial_write_dec(gsi_base);
	serial_write(" max_redir=");
	serial_write_dec(max_redir);
	serial_write("\n");
}

void ioapic_route(uint32_t gsi, uint8_t vector, uint8_t lapic_id,
                  bool active_low, bool level)
{
	if (gsi < gsi_base_g) {
		panic("ioapic_route: GSI below this IOAPIC's base");
	}
	uint32_t index = gsi - gsi_base_g;
	if (index > max_redir) {
		panic("ioapic_route: GSI out of range");
	}

	uint32_t lo = vector;
	if (active_low) {
		lo |= REDIR_ACTIVE_LOW;
	}
	if (level) {
		lo |= REDIR_LEVEL;
	}
	uint32_t hi = (uint32_t)lapic_id << 24U;

	write_redir(index, lo, hi);
}

void ioapic_mask(uint32_t gsi)
{
	if (gsi < gsi_base_g) return;
	uint32_t index = gsi - gsi_base_g;
	if (index > max_redir) return;
	write_redir(index, read_redir_lo(index) | REDIR_MASKED, 0);
}

void ioapic_unmask(uint32_t gsi)
{
	if (gsi < gsi_base_g) return;
	uint32_t index = gsi - gsi_base_g;
	if (index > max_redir) return;
	write_redir(index, read_redir_lo(index) & ~REDIR_MASKED, 0);
}
