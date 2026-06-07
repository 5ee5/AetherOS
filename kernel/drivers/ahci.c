#include "drivers/ahci.h"
#include "drivers/pci.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/serial.h"
#include "lib/string.h"
#include "mem/pmm.h"
#include "mem/vmm.h"

/* ---- AHCI register layout ------------------------------------------------ */

#define AHCI_GHC_AE     (1U << 31)  /* AHCI Enable */
#define AHCI_GHC_HR     (1U << 0)   /* HBA Reset */

#define AHCI_PxCMD_ST   (1U << 0)   /* Start */
#define AHCI_PxCMD_FRE  (1U << 4)   /* FIS Receive Enable */
#define AHCI_PxCMD_FR   (1U << 14)  /* FIS Receive Running */
#define AHCI_PxCMD_CR   (1U << 15)  /* Command List Running */

#define AHCI_PxTFD_BSY  (1U << 7)
#define AHCI_PxTFD_DRQ  (1U << 3)
#define AHCI_PxTFD_ERR  (1U << 0)

#define AHCI_PxSSTS_DET_MASK  0xfU
#define AHCI_PxSSTS_DET_PRES  3U    /* device present and comm established */
#define AHCI_PxSSTS_IPM_SHIFT 8U
#define AHCI_PxSSTS_IPM_ACTIVE 1U

#define AHCI_SIG_ATA    0x00000101U  /* SATA drive */
#define AHCI_SIG_ATAPI  0xeb140101U

#define FIS_TYPE_H2D    0x27U

#define ATA_CMD_READ_DMA_EXT  0x25U

#define MAX_PORTS 32

/* ---- MMIO structures (volatile, packed) ----------------------------------- */

typedef volatile struct {
    uint32_t clba;
    uint32_t clbau;
    uint32_t fb;
    uint32_t fbu;
    uint32_t is;
    uint32_t ie;
    uint32_t cmd;
    uint32_t reserved0;
    uint32_t tfd;
    uint32_t sig;
    uint32_t ssts;
    uint32_t sctl;
    uint32_t serr;
    uint32_t sact;
    uint32_t ci;
    uint32_t sntf;
    uint32_t fbs;
    uint32_t devslp;
    uint8_t  reserved1[40];
    uint8_t  vendor[16];
} __attribute__((packed)) ahci_port_regs_t;

typedef volatile struct {
    uint32_t cap;
    uint32_t ghc;
    uint32_t is;
    uint32_t pi;
    uint32_t vs;
    uint32_t ccc_ctl;
    uint32_t ccc_pts;
    uint32_t em_loc;
    uint32_t em_ctl;
    uint32_t cap2;
    uint32_t bohc;
    uint8_t  reserved[0xa0 - 0x2c];
    uint8_t  vendor[0x100 - 0xa0];
    ahci_port_regs_t ports[MAX_PORTS];
} __attribute__((packed)) ahci_hba_t;

/* ---- DMA structures (non-volatile, must be cache-coherent) ---------------- */

typedef struct {
    uint16_t flags;    /* CFL[4:0], ATAPI, Write, Prefetch, Reset, BIST, Clear, PMP[15:12] */
    uint16_t prdtl;    /* PRD table length in entries */
    uint32_t prdbc;    /* PRD byte count transferred */
    uint32_t ctba;     /* Command table base address low */
    uint32_t ctbau;    /* Command table base address upper */
    uint32_t reserved[4];
} __attribute__((packed)) ahci_cmd_header_t;

typedef struct {
    uint8_t  fis_type;
    uint8_t  pm_port_c; /* bits[3:0]=port, bit[7]=C */
    uint8_t  command;
    uint8_t  feature_lo;
    uint8_t  lba0, lba1, lba2;
    uint8_t  device;
    uint8_t  lba3, lba4, lba5;
    uint8_t  feature_hi;
    uint8_t  count_lo, count_hi;
    uint8_t  icc;
    uint8_t  control;
    uint32_t reserved;
} __attribute__((packed)) ahci_fis_h2d_t;

typedef struct {
    uint32_t dba;      /* data base address low */
    uint32_t dbau;     /* data base address high */
    uint32_t reserved;
    uint32_t dbc;      /* byte count (bit 31 = IRQ, bits 21:0 = count-1) */
} __attribute__((packed)) ahci_prd_t;

typedef struct {
    uint8_t    cfis[64];   /* command FIS */
    uint8_t    acmd[16];   /* ATAPI command */
    uint8_t    reserved[48];
    ahci_prd_t prdt[1];    /* one PRD is enough for one 4K page */
} __attribute__((packed)) ahci_cmd_table_t;

/* ---- Driver state --------------------------------------------------------- */

static ahci_hba_t *s_hba;
static int s_first_disk = -1;

struct port_state {
    bool present;
    ahci_cmd_header_t *cmd_list;   /* virt ptr to 1-page command list */
    uint64_t           cmd_list_phys;
    uint8_t           *fis_buf;    /* virt ptr to 1-page FIS receive area */
    uint64_t           fis_phys;
    ahci_cmd_table_t  *cmd_table;  /* virt ptr to 1-page command table */
    uint64_t           cmd_table_phys;
};

static struct port_state s_ports[MAX_PORTS];

/* ---- Helpers -------------------------------------------------------------- */

static uint64_t direct_base(void)
{
    return vmm_direct_map_base();
}

static void *phys_to_virt_ptr(uint64_t phys)
{
    return (void *)(uintptr_t)(direct_base() + phys);
}

static void port_stop(ahci_port_regs_t *p)
{
    p->cmd &= ~(AHCI_PxCMD_ST | AHCI_PxCMD_FRE);
    /* Wait for FR + CR to clear (up to ~500ms) */
    for (int i = 0; i < 50000; ++i) {
        if (!(p->cmd & (AHCI_PxCMD_FR | AHCI_PxCMD_CR))) break;
        for (volatile int j = 0; j < 1000; ++j) {}
    }
}

static void port_start(ahci_port_regs_t *p)
{
    /* Wait for CR to clear before setting ST. */
    for (int i = 0; i < 50000; ++i) {
        if (!(p->cmd & AHCI_PxCMD_CR)) break;
        for (volatile int j = 0; j < 1000; ++j) {}
    }
    p->cmd |= AHCI_PxCMD_FRE;
    p->cmd |= AHCI_PxCMD_ST;
}

static bool port_is_present(ahci_port_regs_t *p)
{
    uint32_t det = p->ssts & AHCI_PxSSTS_DET_MASK;
    uint32_t ipm = (p->ssts >> AHCI_PxSSTS_IPM_SHIFT) & 0xfU;
    return (det == AHCI_PxSSTS_DET_PRES && ipm == AHCI_PxSSTS_IPM_ACTIVE);
}

/* ---- Port initialisation -------------------------------------------------- */

static bool port_init(uint8_t port_idx)
{
    ahci_port_regs_t *p = &s_hba->ports[port_idx];

    if (!port_is_present(p)) return false;
    if (p->sig != AHCI_SIG_ATA) return false; /* only plain SATA disks */

    port_stop(p);

    /* Allocate a page for the command list (32 headers * 32 bytes = 1024 bytes). */
    uint64_t cl_phys = pmm_alloc_page();
    if (cl_phys == PMM_ALLOC_FAILED) return false;
    ahci_cmd_header_t *cl = phys_to_virt_ptr(cl_phys);
    memset(cl, 0, 4096);

    /* Allocate a page for the FIS receive area. */
    uint64_t fis_phys = pmm_alloc_page();
    if (fis_phys == PMM_ALLOC_FAILED) { pmm_free_page(cl_phys); return false; }
    uint8_t *fis = phys_to_virt_ptr(fis_phys);
    memset(fis, 0, 4096);

    /* Allocate a page for the command table (one slot). */
    uint64_t ct_phys = pmm_alloc_page();
    if (ct_phys == PMM_ALLOC_FAILED) {
        pmm_free_page(cl_phys);
        pmm_free_page(fis_phys);
        return false;
    }
    ahci_cmd_table_t *ct = phys_to_virt_ptr(ct_phys);
    memset(ct, 0, 4096);

    /* Wire command header 0 to the command table. */
    cl[0].ctba  = (uint32_t)(ct_phys & 0xffffffffU);
    cl[0].ctbau = (uint32_t)(ct_phys >> 32);

    /* Program port registers. */
    p->clba  = (uint32_t)(cl_phys & 0xffffffffU);
    p->clbau = (uint32_t)(cl_phys >> 32);
    p->fb    = (uint32_t)(fis_phys & 0xffffffffU);
    p->fbu   = (uint32_t)(fis_phys >> 32);
    p->is    = 0xffffffffU; /* clear interrupts */
    p->serr  = 0xffffffffU; /* clear errors */
    p->ie    = 0;           /* polling mode, no IRQs */

    s_ports[port_idx].present        = true;
    s_ports[port_idx].cmd_list       = cl;
    s_ports[port_idx].cmd_list_phys  = cl_phys;
    s_ports[port_idx].fis_buf        = fis;
    s_ports[port_idx].fis_phys       = fis_phys;
    s_ports[port_idx].cmd_table      = ct;
    s_ports[port_idx].cmd_table_phys = ct_phys;

    port_start(p);

    serial_write("ahci: port ");
    serial_write_dec(port_idx);
    serial_write(" ready\n");
    return true;
}

/* ---- Public API ----------------------------------------------------------- */

bool ahci_init(void)
{
    struct pci_addr addr = pci_find_class(0x01, 0x06); /* Mass Storage, AHCI */
    if (!addr.valid) {
        serial_write("ahci: no AHCI controller found\n");
        return false;
    }

    serial_write("ahci: found at ");
    serial_write_dec(addr.bus);
    serial_write(":");
    serial_write_dec(addr.dev);
    serial_write(".");
    serial_write_dec(addr.func);
    serial_write("\n");

    pci_enable_busmaster(addr);

    uint64_t abar_phys = pci_read_bar(addr, 5);
    if (abar_phys == 0) {
        serial_write("ahci: BAR5 not mapped\n");
        return false;
    }

    s_hba = (ahci_hba_t *)(uintptr_t)(direct_base() + abar_phys);

    /* Enable AHCI mode. */
    s_hba->ghc |= AHCI_GHC_AE;

    uint32_t pi = s_hba->pi;
    bool found = false;

    for (uint8_t i = 0; i < MAX_PORTS; ++i) {
        if (!(pi & (1U << i))) continue;
        if (port_init(i)) {
            if (s_first_disk < 0) s_first_disk = (int)i;
            found = true;
        }
    }

    return found;
}

int ahci_first_disk(void)
{
    return s_first_disk;
}

bool ahci_read_sectors(uint8_t port, uint64_t lba, uint16_t count, void *buf)
{
    if (!s_ports[port].present) return false;
    if (count == 0 || count > 8) return false; /* max 4096 bytes = 8 sectors */

    ahci_port_regs_t  *p  = &s_hba->ports[port];
    ahci_cmd_header_t *cl = s_ports[port].cmd_list;
    ahci_cmd_table_t  *ct = s_ports[port].cmd_table;

    /* Ensure port is not busy. */
    uint32_t tfd = p->tfd;
    if (tfd & (AHCI_PxTFD_BSY | AHCI_PxTFD_DRQ)) return false;

    /* Build PRD entry pointing to caller's buffer.
       The caller must pass a pointer whose physical address we can compute. */
    uint64_t buf_virt = (uint64_t)(uintptr_t)buf;
    uint64_t buf_phys = buf_virt - direct_base();

    uint32_t byte_count = (uint32_t)count * 512;
    ct->prdt[0].dba  = (uint32_t)(buf_phys & 0xffffffffU);
    ct->prdt[0].dbau = (uint32_t)(buf_phys >> 32);
    ct->prdt[0].reserved = 0;
    ct->prdt[0].dbc  = (byte_count - 1) & 0x3fffffU; /* no IRQ */

    /* Build H2D FIS in the command table. */
    ahci_fis_h2d_t *fis = (ahci_fis_h2d_t *)ct->cfis;
    memset(ct->cfis, 0, 64);
    fis->fis_type   = FIS_TYPE_H2D;
    fis->pm_port_c  = 0x80U; /* C=1: command */
    fis->command    = ATA_CMD_READ_DMA_EXT;
    fis->device     = 0x40U; /* LBA mode */
    fis->lba0       = (uint8_t)(lba);
    fis->lba1       = (uint8_t)(lba >> 8);
    fis->lba2       = (uint8_t)(lba >> 16);
    fis->lba3       = (uint8_t)(lba >> 24);
    fis->lba4       = (uint8_t)(lba >> 32);
    fis->lba5       = (uint8_t)(lba >> 40);
    fis->count_lo   = (uint8_t)(count);
    fis->count_hi   = (uint8_t)(count >> 8);

    /* Build command header (slot 0). */
    cl[0].flags  = (5U) /* CFL=5 DWORDs for H2D */
                 | (0U << 6) /* read, not write */;
    cl[0].prdtl  = 1;
    cl[0].prdbc  = 0;

    /* Issue command in slot 0. */
    p->is = 0xffffffffU;
    p->ci = 1U;

    /* Poll until slot 0 is cleared or error. */
    for (uint32_t i = 0; i < 1000000U; ++i) {
        if (!(p->ci & 1U)) {
            /* Check for error. */
            if (p->is & (1U << 30)) return false; /* task file error */
            return true;
        }
        if (p->tfd & AHCI_PxTFD_ERR) return false;
        for (volatile int j = 0; j < 10; ++j) {}
    }

    serial_write("ahci: read timeout\n");
    return false;
}
