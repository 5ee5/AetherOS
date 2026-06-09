#include "acpi/acpi.h"

#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/io.h"
#include "core/panic.h"
#include "core/serial.h"

/* ---- ACPI table structures -------------------------------------------- */

struct rsdp {
	char     signature[8];   /* "RSD PTR " */
	uint8_t  checksum;
	char     oem_id[6];
	uint8_t  revision;       /* 0 = ACPI 1.0, 2 = ACPI 2.0+ */
	uint32_t rsdt_address;
	/* ACPI 2.0+ fields */
	uint32_t length;
	uint64_t xsdt_address;
	uint8_t  extended_checksum;
	uint8_t  reserved[3];
} __attribute__((packed));

struct sdt_header {
	char     signature[4];
	uint32_t length;
	uint8_t  revision;
	uint8_t  checksum;
	char     oem_id[6];
	char     oem_table_id[8];
	uint32_t oem_revision;
	uint32_t creator_id;
	uint32_t creator_revision;
} __attribute__((packed));

struct xsdt {
	struct sdt_header header;
	/* Followed by (length - sizeof(header)) / 8 uint64_t physical addresses. */
} __attribute__((packed));

struct madt {
	struct sdt_header header;
	uint32_t local_apic_address;
	uint32_t flags;
	/* Followed by variable-length interrupt controller structures. */
} __attribute__((packed));

/* Fixed ACPI Description Table (FACP) — key fields only. */
struct fadt {
	struct sdt_header header;   /* 0 — 36 bytes */
	uint32_t firmware_ctrl;     /* 36 */
	uint32_t dsdt;              /* 40 — 32-bit physical address of DSDT */
	uint8_t  pad1[20];          /* 44 — skip to pm1a_cnt_blk */
	uint32_t pm1a_cnt_blk;      /* 64 — I/O port for PM1a control (16-bit wide) */
	uint32_t pm1b_cnt_blk;      /* 68 — I/O port for PM1b control, 0 if absent */
	uint8_t  pad2[44];          /* 72 — skip to reset_reg at 116 */
	/* ACPI 2.0+ reset register (Generic Address Structure = 12 bytes): */
	uint8_t  reset_reg_space;   /* 116 — 0=memory, 1=I/O */
	uint8_t  reset_reg_bw;      /* 117 */
	uint8_t  reset_reg_bo;      /* 118 */
	uint8_t  reset_reg_acc;     /* 119 */
	uint64_t reset_reg_addr;    /* 120 */
	uint8_t  reset_value;       /* 128 */
} __attribute__((packed));

/* MADT interrupt controller structure types. */
#define MADT_TYPE_LOCAL_APIC  0
#define MADT_TYPE_IOAPIC      1
#define MADT_TYPE_ISO         2   /* interrupt source override */

struct madt_record_header {
	uint8_t type;
	uint8_t length;
} __attribute__((packed));

struct madt_ioapic {
	struct madt_record_header header;
	uint8_t  id;
	uint8_t  reserved;
	uint32_t address;
	uint32_t gsi_base;
} __attribute__((packed));

struct madt_local_apic {
	struct madt_record_header header;
	uint8_t  acpi_processor_id;
	uint8_t  apic_id;
	uint32_t flags;   /* bit 0: enabled, bit 1: online capable */
} __attribute__((packed));

/* ---- Module state ----------------------------------------------------- */

static uint64_t dm_base;
static struct acpi_madt_info madt_info;
static bool madt_valid;

/* FADT-derived state for poweroff / reboot */
static uint16_t s_pm1a_cnt;    /* I/O port; 0 = not found */
static uint16_t s_pm1b_cnt;    /* I/O port; 0 = absent */
static uint8_t  s_slp_typ_s5 = 5u; /* default covers QEMU; updated by DSDT scan */
static bool     s_fadt_valid;

/* ---- Helpers ---------------------------------------------------------- */

static void *phys(uint64_t addr)
{
	return (void *)(uintptr_t)(dm_base + addr);
}

static bool verify_checksum(const void *table, uint32_t length)
{
	const uint8_t *p = (const uint8_t *)table;
	uint8_t sum = 0;
	for (uint32_t i = 0; i < length; ++i) {
		sum = (uint8_t)(sum + p[i]);
	}
	return sum == 0;
}

static void parse_madt(const struct madt *m)
{
	if (!verify_checksum(m, m->header.length)) {
		serial_write("acpi: MADT checksum invalid, skipping\n");
		return;
	}

	madt_info.local_apic_base = m->local_apic_address;
	madt_info.ioapic_found    = false;
	madt_info.cpu_count       = 0;

	const uint8_t *ptr = (const uint8_t *)(m + 1);
	const uint8_t *end = (const uint8_t *)m + m->header.length;

	while (ptr + sizeof(struct madt_record_header) <= end) {
		const struct madt_record_header *rec =
			(const struct madt_record_header *)ptr;
		if (rec->length < sizeof(struct madt_record_header) ||
			ptr + rec->length > end) {
			break;
		}

		if (rec->type == MADT_TYPE_LOCAL_APIC) {
			const struct madt_local_apic *la =
				(const struct madt_local_apic *)ptr;
			if ((la->flags & 0x3U) != 0 &&
				madt_info.cpu_count < ACPI_MAX_CPUS) {
				madt_info.cpu_lapic_ids[madt_info.cpu_count++] =
					la->apic_id;
			}
		} else if (rec->type == MADT_TYPE_IOAPIC && !madt_info.ioapic_found) {
			const struct madt_ioapic *io =
				(const struct madt_ioapic *)ptr;
			madt_info.ioapic_base     = io->address;
			madt_info.ioapic_id       = io->id;
			madt_info.ioapic_gsi_base = io->gsi_base;
			madt_info.ioapic_found    = true;
		}

		ptr += rec->length;
	}

	madt_valid = true;

	serial_write("acpi: lapic_base=");
	serial_write_hex(madt_info.local_apic_base);
	serial_write(" cpus=");
	serial_write_dec(madt_info.cpu_count);
	if (madt_info.ioapic_found) {
		serial_write(" ioapic_base=");
		serial_write_hex(madt_info.ioapic_base);
		serial_write(" gsi_base=");
		serial_write_dec(madt_info.ioapic_gsi_base);
	}
	serial_write("\n");
}

static void parse_fadt(const struct fadt *f)
{
	if (!verify_checksum(f, f->header.length)) {
		serial_write("acpi: FADT checksum invalid, skipping\n");
		return;
	}

	s_pm1a_cnt = (uint16_t)(f->pm1a_cnt_blk & 0xffffu);
	s_pm1b_cnt = (uint16_t)(f->pm1b_cnt_blk & 0xffffu);

	/* Best-effort _S5_ scan in the DSDT to find SLP_TYP for S5 sleep.
	   AML: NameOp "_S5_" PackageOp PkgLength NumElems BytePrefix SLP_TYP ...
	   We look for the "_S5_" bytes then find the first 0x0A (BytePrefix) within
	   14 bytes and take the following byte as SLP_TYP_S5. */
	if (f->dsdt != 0) {
		const struct sdt_header *dsdt =
			(const struct sdt_header *)phys((uint64_t)f->dsdt);
		const uint8_t *d = (const uint8_t *)dsdt;
		uint32_t len = dsdt->length;
		for (uint32_t i = 4; i + 14 < len; ++i) {
			if (d[i] == '_' && d[i+1] == 'S' &&
			    d[i+2] == '5' && d[i+3] == '_') {
				for (uint32_t j = i + 4; j < i + 18 && j + 1 < len; ++j) {
					if (d[j] == 0x0au) {
						s_slp_typ_s5 = d[j + 1];
						break;
					}
				}
				break;
			}
		}
	}

	s_fadt_valid = true;
	serial_write("acpi: FADT pm1a_cnt=");
	serial_write_hex(s_pm1a_cnt);
	serial_write(" slp_typ_s5=");
	serial_write_dec(s_slp_typ_s5);
	serial_write("\n");
}

static void scan_sdt_entries(const void *entries, uint32_t count,
                              uint32_t entry_size)
{
	bool found_madt = false;
	const uint8_t *p = (const uint8_t *)entries;
	for (uint32_t i = 0; i < count; ++i, p += entry_size) {
		uint64_t addr;
		if (entry_size == 8U) {
			uint64_t v; __builtin_memcpy(&v, p, 8); addr = v;
		} else {
			uint32_t v; __builtin_memcpy(&v, p, 4); addr = v;
		}
		const struct sdt_header *hdr =
			(const struct sdt_header *)phys(addr);
		if (hdr->signature[0] == 'A' && hdr->signature[1] == 'P' &&
			hdr->signature[2] == 'I' && hdr->signature[3] == 'C') {
			parse_madt((const struct madt *)hdr);
			found_madt = true;
		} else if (hdr->signature[0] == 'F' && hdr->signature[1] == 'A' &&
			hdr->signature[2] == 'C' && hdr->signature[3] == 'P') {
			parse_fadt((const struct fadt *)hdr);
		}
	}
	if (!found_madt)
		serial_write("acpi: MADT not found\n");
}

static void parse_xsdt(const struct xsdt *x)
{
	if (!verify_checksum(x, x->header.length)) {
		panic("acpi: XSDT checksum invalid");
	}
	uint32_t count =
		(x->header.length - (uint32_t)sizeof(struct sdt_header)) / 8U;
	scan_sdt_entries(x + 1, count, 8U);
}

static void parse_rsdt(const struct sdt_header *r)
{
	if (!verify_checksum(r, r->length)) {
		panic("acpi: RSDT checksum invalid");
	}
	uint32_t count =
		(r->length - (uint32_t)sizeof(struct sdt_header)) / 4U;
	scan_sdt_entries(r + 1, count, 4U);
}

/* ---- Public API ------------------------------------------------------- */

void acpi_init(uint64_t rsdp_phys, uint64_t direct_map_base)
{
	dm_base    = direct_map_base;
	madt_valid = false;

	if (rsdp_phys == 0) {
		serial_write("acpi: no RSDP from bootloader\n");
		return;
	}

	const struct rsdp *rsdp = (const struct rsdp *)phys(rsdp_phys);

	if (rsdp->signature[0] != 'R' || rsdp->signature[1] != 'S' ||
		rsdp->signature[2] != 'D' || rsdp->signature[3] != ' ' ||
		rsdp->signature[4] != 'P' || rsdp->signature[5] != 'T' ||
		rsdp->signature[6] != 'R' || rsdp->signature[7] != ' ') {
		panic("acpi: invalid RSDP signature");
	}

	serial_write("acpi: RSDP rev=");
	serial_write_dec(rsdp->revision);
	serial_write("\n");

	if (rsdp->revision >= 2 && rsdp->xsdt_address != 0) {
		const struct xsdt *x =
			(const struct xsdt *)phys(rsdp->xsdt_address);
		if (x->header.signature[0] != 'X' ||
			x->header.signature[1] != 'S' ||
			x->header.signature[2] != 'D' ||
			x->header.signature[3] != 'T') {
			panic("acpi: invalid XSDT signature");
		}
		parse_xsdt(x);
	} else {
		const struct sdt_header *r =
			(const struct sdt_header *)phys(rsdp->rsdt_address);
		if (r->signature[0] != 'R' || r->signature[1] != 'S' ||
			r->signature[2] != 'D' || r->signature[3] != 'T') {
			panic("acpi: invalid RSDT signature");
		}
		parse_rsdt(r);
	}
}

const struct acpi_madt_info *acpi_madt(void)
{
	if (!madt_valid) {
		return NULL;
	}
	return &madt_info;
}

void acpi_poweroff(void)
{
	if (s_fadt_valid && s_pm1a_cnt != 0u) {
		uint16_t slp_val = (uint16_t)(((uint16_t)s_slp_typ_s5 << 10) | (1u << 13));
		outw(s_pm1a_cnt, slp_val);
		if (s_pm1b_cnt != 0u)
			outw(s_pm1b_cnt, slp_val);
	}
	for (;;) __asm__ volatile("hlt");
}

void acpi_reboot(void)
{
	outb(0xCF9u, 0x06u);   /* PCI hard reset */
	for (;;) __asm__ volatile("hlt");
}
