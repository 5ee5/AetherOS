#include "drivers/nvme.h"
#include "drivers/pci.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/serial.h"
#include "lib/string.h"
#include "mem/pmm.h"
#include "mem/vmm.h"

/* Fixed virtual address for NVMe MMIO — sits just above the IOAPIC mapping.
   We map NVME_MMIO_PAGES pages here during nvme_init. */
#define NVME_MMIO_VIRT  0xffffa00000010000ULL
#define NVME_MMIO_PAGES 8u   /* 32 KiB — covers registers + all doorbells */

/* ---- Controller register offsets ----------------------------------------- */

#define NVME_REG_CAP    0x00u
#define NVME_REG_VS     0x08u
#define NVME_REG_CC     0x14u
#define NVME_REG_CSTS   0x1Cu
#define NVME_REG_AQA    0x24u
#define NVME_REG_ASQ    0x28u
#define NVME_REG_ACQ    0x30u

#define NVME_CC_EN          (1u << 0)
#define NVME_CC_CSS_NVM     (0u << 4)
#define NVME_CC_MPS(n)      ((uint32_t)(n) << 7)
#define NVME_CC_IOSQES(n)   ((uint32_t)(n) << 16)
#define NVME_CC_IOCQES(n)   ((uint32_t)(n) << 20)

#define NVME_CSTS_RDY   (1u << 0)
#define NVME_CSTS_CFS   (1u << 1)

/* ---- Queue sizing --------------------------------------------------------- */

#define NVME_AQ_SIZE    64u
#define NVME_IOQ_SIZE   64u

/* ---- Opcodes -------------------------------------------------------------- */

#define NVME_ADM_CREATE_IOCQ    0x05u
#define NVME_ADM_CREATE_IOSQ    0x01u
#define NVME_ADM_IDENTIFY       0x06u

#define NVME_IO_WRITE   0x01u
#define NVME_IO_READ    0x02u

#define NVME_CNS_IDENTIFY_NS    0x00u

/* ---- Queue entry types ---------------------------------------------------- */

typedef struct {
    uint32_t cdw0;   /* OPC[7:0] | FUSE[9:8] | PSDT[15:14] | CID[31:16] */
    uint32_t nsid;
    uint32_t cdw2;
    uint32_t cdw3;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} __attribute__((packed)) nvme_sqe_t;

typedef struct {
    uint32_t dw0;     /* command-specific result */
    uint32_t dw1;
    uint16_t sqhd;
    uint16_t sqid;
    uint16_t cid;
    uint16_t status;  /* bits[15:1] = SC+SCT, bit[0] = phase tag */
} __attribute__((packed)) nvme_cqe_t;

/* ---- Driver state --------------------------------------------------------- */

static volatile uint8_t *s_regs;
static uint32_t s_dstrd;    /* doorbell stride in bytes */

/* admin queues */
static nvme_sqe_t *s_asq;
static nvme_cqe_t *s_acq;
static uint64_t    s_asq_phys;
static uint64_t    s_acq_phys;
static uint16_t    s_asq_tail;
static uint16_t    s_acq_head;
static uint8_t     s_acq_phase;

/* I/O queue 1 */
static nvme_sqe_t *s_iosq;
static nvme_cqe_t *s_iocq;
static uint64_t    s_iosq_phys;
static uint64_t    s_iocq_phys;
static uint16_t    s_iosq_tail;
static uint16_t    s_iocq_head;
static uint8_t     s_iocq_phase;

static uint16_t s_next_cid;
static bool     s_ready;

/* ---- Register I/O --------------------------------------------------------- */

static uint32_t reg32(uint32_t off)
{
    return *(volatile uint32_t *)(s_regs + off);
}

static uint64_t reg64(uint32_t off)
{
    return (uint64_t)*(volatile uint32_t *)(s_regs + off) |
           ((uint64_t)*(volatile uint32_t *)(s_regs + off + 4) << 32);
}

static void wreg32(uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(s_regs + off) = v;
}

static void wreg64(uint32_t off, uint64_t v)
{
    *(volatile uint32_t *)(s_regs + off)     = (uint32_t)(v);
    *(volatile uint32_t *)(s_regs + off + 4) = (uint32_t)(v >> 32);
}

/* Doorbell for queue slot qid (0=admin SQ, 1=admin CQ, 2=I/O SQ, 3=I/O CQ) */
static void doorbell(uint32_t qid, uint32_t val)
{
    *(volatile uint32_t *)(s_regs + 0x1000u + qid * s_dstrd) = val;
}

/* ---- Polling helpers ------------------------------------------------------ */

static bool wait_csts(uint32_t want, uint32_t iters)
{
    for (uint32_t i = 0; i < iters; ++i) {
        uint32_t csts = reg32(NVME_REG_CSTS);
        if (csts & NVME_CSTS_CFS) return false;
        if ((csts & NVME_CSTS_RDY) == want) return true;
        for (volatile int j = 0; j < 1000; ++j) {}
    }
    return false;
}

/* ---- Admin queue mechanics ----------------------------------------------- */

static uint16_t asq_submit(nvme_sqe_t *cmd)
{
    uint16_t cid = s_next_cid++;
    cmd->cdw0 = (cmd->cdw0 & 0x0000ffffu) | ((uint32_t)cid << 16);
    s_asq[s_asq_tail] = *cmd;
    s_asq_tail = (uint16_t)((s_asq_tail + 1u) % NVME_AQ_SIZE);
    doorbell(0, s_asq_tail);
    return cid;
}

static bool acq_wait(void)
{
    for (uint32_t i = 0; i < 2000000u; ++i) {
        nvme_cqe_t *e = &s_acq[s_acq_head];
        if ((e->status & 1u) != s_acq_phase) {
            for (volatile int j = 0; j < 10; ++j) {}
            continue;
        }
        bool ok = ((e->status >> 1) & 0x7ffu) == 0;
        s_acq_head = (uint16_t)((s_acq_head + 1u) % NVME_AQ_SIZE);
        if (s_acq_head == 0) s_acq_phase ^= 1u;
        doorbell(1, s_acq_head);
        return ok;
    }
    serial_write("nvme: admin timeout\n");
    return false;
}

static bool admin_cmd(nvme_sqe_t *cmd)
{
    asq_submit(cmd);
    return acq_wait();
}

/* ---- Admin commands ------------------------------------------------------- */

static bool cmd_identify_ns(uint64_t buf_phys)
{
    nvme_sqe_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0  = NVME_ADM_IDENTIFY;
    cmd.nsid  = 1;
    cmd.prp1  = buf_phys;
    cmd.cdw10 = NVME_CNS_IDENTIFY_NS;
    return admin_cmd(&cmd);
}

static bool cmd_create_iocq(uint64_t phys)
{
    nvme_sqe_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0  = NVME_ADM_CREATE_IOCQ;
    cmd.prp1  = phys;
    cmd.cdw10 = ((uint32_t)(NVME_IOQ_SIZE - 1u) << 16) | 1u;  /* QSIZE | QID=1 */
    cmd.cdw11 = 1u;   /* PC=1 (physically contiguous) */
    return admin_cmd(&cmd);
}

static bool cmd_create_iosq(uint64_t phys)
{
    nvme_sqe_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0  = NVME_ADM_CREATE_IOSQ;
    cmd.prp1  = phys;
    cmd.cdw10 = ((uint32_t)(NVME_IOQ_SIZE - 1u) << 16) | 1u;  /* QSIZE | QID=1 */
    cmd.cdw11 = (1u << 16) | 1u;   /* CQID=1 | PC=1 */
    return admin_cmd(&cmd);
}

/* ---- I/O queue mechanics -------------------------------------------------- */

static bool io_cmd(uint8_t opc, uint64_t slba, uint16_t nlb, uint64_t buf_phys)
{
    nvme_sqe_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    uint16_t cid = s_next_cid++;
    cmd.cdw0  = (uint32_t)opc | ((uint32_t)cid << 16);
    cmd.nsid  = 1;
    cmd.prp1  = buf_phys;
    cmd.cdw10 = (uint32_t)(slba & 0xffffffffu);
    cmd.cdw11 = (uint32_t)(slba >> 32);
    cmd.cdw12 = (uint32_t)(nlb - 1u);   /* NLB is 0-based */

    s_iosq[s_iosq_tail] = cmd;
    s_iosq_tail = (uint16_t)((s_iosq_tail + 1u) % NVME_IOQ_SIZE);
    doorbell(2, s_iosq_tail);

    for (uint32_t i = 0; i < 2000000u; ++i) {
        nvme_cqe_t *e = &s_iocq[s_iocq_head];
        if ((e->status & 1u) != s_iocq_phase) {
            for (volatile int j = 0; j < 10; ++j) {}
            continue;
        }
        bool ok = ((e->status >> 1) & 0x7ffu) == 0;
        s_iocq_head = (uint16_t)((s_iocq_head + 1u) % NVME_IOQ_SIZE);
        if (s_iocq_head == 0) s_iocq_phase ^= 1u;
        doorbell(3, s_iocq_head);
        return ok;
    }
    serial_write("nvme: I/O timeout\n");
    return false;
}

/* ---- Public API ----------------------------------------------------------- */

bool nvme_read_sectors(uint64_t lba, uint16_t count, void *buf)
{
    if (!s_ready || count == 0u || count > 8u) return false;
    uint64_t phys = (uint64_t)(uintptr_t)buf - vmm_direct_map_base();
    return io_cmd(NVME_IO_READ, lba, count, phys);
}

bool nvme_write_sectors(uint64_t lba, uint16_t count, const void *buf)
{
    if (!s_ready || count == 0u || count > 8u) return false;
    uint64_t phys = (uint64_t)(uintptr_t)buf - vmm_direct_map_base();
    return io_cmd(NVME_IO_WRITE, lba, count, phys);
}

int nvme_first_ns(void)
{
    return s_ready ? 1 : -1;
}

bool nvme_init(void)
{
    struct pci_addr addr = pci_find_class(0x01u, 0x08u);
    if (!addr.valid) return false;

    serial_write("nvme: found at ");
    serial_write_dec(addr.bus);
    serial_write(":");
    serial_write_dec(addr.dev);
    serial_write(".");
    serial_write_dec(addr.func);
    serial_write("\n");

    pci_enable_busmaster(addr);

    uint64_t bar0 = pci_read_bar(addr, 0);
    if (bar0 == 0) { serial_write("nvme: BAR0 not mapped\n"); return false; }

    /* BAR0 may be beyond the 64 GiB direct map (QEMU assigns it at ~768 GiB).
       Explicitly map it at a reserved kernel virtual address. */
    for (uint32_t pg = 0; pg < NVME_MMIO_PAGES; ++pg)
        vmm_map(NVME_MMIO_VIRT + (uint64_t)pg * 4096u,
                bar0 + (uint64_t)pg * 4096u,
                VMM_WRITABLE | 0x010ULL /* PCD: uncacheable */);
    s_regs = (volatile uint8_t *)(uintptr_t)NVME_MMIO_VIRT;

    uint64_t cap = reg64(NVME_REG_CAP);
    s_dstrd = 4u << ((cap >> 32) & 0xfu);

    /* TO field: bits 31:24, units of 500 ms */
    uint32_t to_iters = (uint32_t)(((cap >> 24) & 0xffu) + 1u) * 50000u;

    /* Reset: clear EN and wait for RDY=0 */
    uint32_t cc = reg32(NVME_REG_CC);
    if (cc & NVME_CC_EN) {
        wreg32(NVME_REG_CC, cc & ~NVME_CC_EN);
        if (!wait_csts(0, to_iters)) {
            serial_write("nvme: reset timeout\n");
            return false;
        }
    }

    /* Admin queues */
    s_asq_phys = pmm_alloc_page();
    s_acq_phys = pmm_alloc_page();
    if (s_asq_phys == PMM_ALLOC_FAILED || s_acq_phys == PMM_ALLOC_FAILED) {
        serial_write("nvme: admin queue alloc failed\n");
        return false;
    }
    s_asq = (nvme_sqe_t *)(uintptr_t)(vmm_direct_map_base() + s_asq_phys);
    s_acq = (nvme_cqe_t *)(uintptr_t)(vmm_direct_map_base() + s_acq_phys);
    memset(s_asq, 0, 4096);
    memset(s_acq, 0, 4096);
    s_asq_tail  = 0;
    s_acq_head  = 0;
    s_acq_phase = 1;   /* first completions from controller have phase=1 */

    wreg64(NVME_REG_ASQ, s_asq_phys);
    wreg64(NVME_REG_ACQ, s_acq_phys);
    wreg32(NVME_REG_AQA,
        ((uint32_t)(NVME_AQ_SIZE - 1u) << 16) | (NVME_AQ_SIZE - 1u));

    /* Enable with: NVM command set, 4 KiB pages, SQES=64B, CQES=16B */
    wreg32(NVME_REG_CC,
        NVME_CC_EN       |
        NVME_CC_CSS_NVM  |
        NVME_CC_MPS(0)   |
        NVME_CC_IOSQES(6)|
        NVME_CC_IOCQES(4));

    if (!wait_csts(NVME_CSTS_RDY, to_iters)) {
        serial_write("nvme: enable timeout\n");
        return false;
    }

    /* Identify namespace 1 — confirm it exists and uses 512B sectors */
    uint64_t id_phys = pmm_alloc_page();
    if (id_phys == PMM_ALLOC_FAILED) {
        serial_write("nvme: identify alloc failed\n");
        return false;
    }
    uint8_t *id = (uint8_t *)(uintptr_t)(vmm_direct_map_base() + id_phys);
    memset(id, 0, 4096);

    if (!cmd_identify_ns(id_phys)) {
        serial_write("nvme: identify namespace failed\n");
        pmm_free_page(id_phys);
        return false;
    }

    uint64_t nsze;
    memcpy(&nsze, id, 8);

    /* FLBAS[3:0] = current LBA format index; LBAF at offset 128, 4 bytes each */
    uint8_t flbas = id[26] & 0x0fu;
    uint8_t lbads = id[128 + flbas * 4 + 2]; /* LBADS: sector size = 2^lbads */
    pmm_free_page(id_phys);

    if (lbads != 9u) {
        serial_write("nvme: unsupported sector size (lbads=");
        serial_write_dec(lbads);
        serial_write(", need 9 for 512B)\n");
        return false;
    }

    serial_write("nvme: ns1 size=");
    serial_write_hex(nsze);
    serial_write(" sectors\n");

    /* Create I/O completion queue 1, then submission queue 1 */
    s_iocq_phys = pmm_alloc_page();
    s_iosq_phys = pmm_alloc_page();
    if (s_iocq_phys == PMM_ALLOC_FAILED || s_iosq_phys == PMM_ALLOC_FAILED) {
        serial_write("nvme: I/O queue alloc failed\n");
        return false;
    }
    s_iocq = (nvme_cqe_t *)(uintptr_t)(vmm_direct_map_base() + s_iocq_phys);
    s_iosq = (nvme_sqe_t *)(uintptr_t)(vmm_direct_map_base() + s_iosq_phys);
    memset(s_iocq, 0, 4096);
    memset(s_iosq, 0, 4096);
    s_iocq_head  = 0;
    s_iosq_tail  = 0;
    s_iocq_phase = 1;

    if (!cmd_create_iocq(s_iocq_phys)) {
        serial_write("nvme: create IOCQ failed\n");
        return false;
    }
    if (!cmd_create_iosq(s_iosq_phys)) {
        serial_write("nvme: create IOSQ failed\n");
        return false;
    }

    serial_write("nvme: ready\n");
    s_ready = true;
    return true;
}
