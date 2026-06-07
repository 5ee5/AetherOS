#include "drivers/e1000.h"
#include "drivers/pci.h"
#include "net/nic.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/serial.h"
#include "lib/string.h"
#include "mem/pmm.h"
#include "mem/vmm.h"

/* ---- e1000 register offsets ---------------------------------------------- */
#define E1000_CTRL    0x0000U
#define E1000_STATUS  0x0008U
#define E1000_EECD    0x0010U
#define E1000_EERD    0x0014U
#define E1000_ICR     0x00C0U
#define E1000_IMS     0x00D0U
#define E1000_IMC     0x00D8U
#define E1000_RCTL    0x0100U
#define E1000_TCTL    0x0400U
#define E1000_TIPG    0x0410U
#define E1000_RDBAL   0x2800U
#define E1000_RDBAH   0x2804U
#define E1000_RDLEN   0x2808U
#define E1000_RDH     0x2810U
#define E1000_RDT     0x2818U
#define E1000_TDBAL   0x3800U
#define E1000_TDBAH   0x3804U
#define E1000_TDLEN   0x3808U
#define E1000_TDH     0x3810U
#define E1000_TDT     0x3818U
#define E1000_RAL0    0x5400U
#define E1000_RAH0    0x5404U
#define E1000_MTA     0x5200U  /* Multicast Table Array (128 entries) */

/* CTRL bits */
#define CTRL_SLU      (1U << 6)   /* Set Link Up */
#define CTRL_RST      (1U << 26)  /* Device Reset */

/* RCTL bits */
#define RCTL_EN       (1U << 1)
#define RCTL_UPE      (1U << 3)   /* Unicast Promisc */
#define RCTL_MPE      (1U << 4)   /* Multicast Promisc */
#define RCTL_LPE      (1U << 5)   /* Long Packet Enable */
#define RCTL_BAM      (1U << 15)  /* Broadcast Accept Mode */
#define RCTL_BSIZE_2K 0U          /* 2K buffer size (bits 17:16 = 00) */
#define RCTL_SECRC    (1U << 26)  /* Strip Ethernet CRC */

/* TCTL bits */
#define TCTL_EN       (1U << 1)
#define TCTL_PSP      (1U << 3)   /* Pad Short Packets */
#define TCTL_CT_DEF   (0x10U << 4)
#define TCTL_COLD_DEF (0x40U << 12)

/* TX descriptor CMD bits */
#define TX_CMD_EOP    0x01U  /* End of Packet */
#define TX_CMD_IFCS   0x02U  /* Insert FCS */
#define TX_CMD_RS     0x08U  /* Report Status */

/* RX/TX descriptor status */
#define DESC_STATUS_DD  0x01U  /* Descriptor Done */
#define DESC_STATUS_EOP 0x02U  /* End of Packet */

/* Ring sizes (must be multiples of 8) */
#define RX_RING_SIZE  32U
#define TX_RING_SIZE  32U
#define RX_BUF_SIZE   2048U

/* ---- descriptor types ---------------------------------------------------- */

typedef struct {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed)) rx_desc_t;

typedef struct {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed)) tx_desc_t;

/* ---- driver state --------------------------------------------------------- */

static volatile uint32_t *s_mmio;
static uint64_t           s_dmap;

static rx_desc_t *s_rx_ring;
static uint64_t   s_rx_ring_phys;
static uint8_t   *s_rx_bufs[RX_RING_SIZE];
static uint64_t   s_rx_buf_phys[RX_RING_SIZE];
static uint32_t   s_rx_tail;

static tx_desc_t *s_tx_ring;
static uint64_t   s_tx_ring_phys;
static uint8_t   *s_tx_buf;   /* one shared 4096-byte TX buffer */
static uint64_t   s_tx_buf_phys;
static uint32_t   s_tx_tail;

static nic_t      s_nic;

/* ---- MMIO helpers --------------------------------------------------------- */

static uint32_t e_read(uint32_t reg)
{
    return s_mmio[reg / 4];
}

static void e_write(uint32_t reg, uint32_t val)
{
    s_mmio[reg / 4] = val;
}

static void *pv(uint64_t phys)
{
    return (void *)(uintptr_t)(s_dmap + phys);
}

/* ---- NIC ops ------------------------------------------------------------- */

static bool e1000_nic_send(nic_t *self, const void *data, uint16_t len)
{
    (void)self;
    if (len > 1518) return false;

    /* Copy into TX buffer. */
    memcpy(s_tx_buf, data, len);

    uint32_t idx = s_tx_tail;
    s_tx_ring[idx].addr   = s_tx_buf_phys;
    s_tx_ring[idx].length = len;
    s_tx_ring[idx].cmd    = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
    s_tx_ring[idx].status = 0;

    s_tx_tail = (idx + 1) % TX_RING_SIZE;
    e_write(E1000_TDT, s_tx_tail);

    /* Poll for completion. */
    for (uint32_t i = 0; i < 100000U; ++i) {
        if (s_tx_ring[idx].status & DESC_STATUS_DD) return true;
        for (volatile int j = 0; j < 10; ++j) {}
    }
    return false;
}

static uint16_t e1000_nic_recv(nic_t *self, void *buf, uint16_t max_len)
{
    (void)self;
    uint32_t idx = (e_read(E1000_RDH) + RX_RING_SIZE - 1) % RX_RING_SIZE;
    /* Walk from our tail to head. */
    uint32_t tail = s_rx_tail;
    if (tail == (e_read(E1000_RDH) % RX_RING_SIZE)) return 0;

    rx_desc_t *d = &s_rx_ring[tail];
    if (!(d->status & DESC_STATUS_DD)) return 0;
    if (!(d->status & DESC_STATUS_EOP)) { /* drop multi-fragment packets */
        d->status = 0;
        d->addr   = s_rx_buf_phys[tail];
        s_rx_tail = (tail + 1) % RX_RING_SIZE;
        e_write(E1000_RDT, tail);
        return 0;
    }

    uint16_t len = d->length;
    if (len > max_len) len = max_len;
    memcpy(buf, s_rx_bufs[tail], len);

    /* Give the descriptor back to hardware. */
    d->status = 0;
    d->addr   = s_rx_buf_phys[tail];
    s_rx_tail = (tail + 1) % RX_RING_SIZE;
    e_write(E1000_RDT, (s_rx_tail + RX_RING_SIZE - 1) % RX_RING_SIZE);

    (void)idx;
    return len;
}

/* ---- Initialisation ------------------------------------------------------- */

bool e1000_init(void)
{
    /* Find e1000: PCI class 0x02 (Network), subclass 0x00 (Ethernet). */
    struct pci_addr pci = pci_find_class(0x02, 0x00);
    if (!pci.valid) {
        serial_write("e1000: no Ethernet controller found\n");
        return false;
    }

    serial_write("e1000: found at ");
    serial_write_dec(pci.bus); serial_write(":");
    serial_write_dec(pci.dev); serial_write(".");
    serial_write_dec(pci.func); serial_write("\n");

    pci_enable_busmaster(pci);

    uint64_t bar0 = pci_read_bar(pci, 0);
    if (bar0 == 0) {
        serial_write("e1000: BAR0 not mapped\n");
        return false;
    }

    s_dmap  = vmm_direct_map_base();
    s_mmio  = (volatile uint32_t *)(uintptr_t)(s_dmap + bar0);

    /* Reset the device. */
    e_write(E1000_CTRL, e_read(E1000_CTRL) | CTRL_RST);
    for (volatile int i = 0; i < 100000; ++i) {}
    while (e_read(E1000_CTRL) & CTRL_RST) {}

    /* Set link up. */
    e_write(E1000_CTRL, e_read(E1000_CTRL) | CTRL_SLU);

    /* Mask all interrupts. */
    e_write(E1000_IMC, 0xffffffffU);
    (void)e_read(E1000_ICR);

    /* Clear multicast table. */
    for (uint32_t i = 0; i < 128; ++i)
        e_write(E1000_MTA + i * 4, 0);

    /* Read MAC from RAL0/RAH0. */
    uint32_t ral = e_read(E1000_RAL0);
    uint32_t rah = e_read(E1000_RAH0);
    s_nic.mac[0] = (uint8_t)(ral);
    s_nic.mac[1] = (uint8_t)(ral >> 8);
    s_nic.mac[2] = (uint8_t)(ral >> 16);
    s_nic.mac[3] = (uint8_t)(ral >> 24);
    s_nic.mac[4] = (uint8_t)(rah);
    s_nic.mac[5] = (uint8_t)(rah >> 8);

    serial_write("e1000: MAC ");
    for (int i = 0; i < 6; ++i) {
        if (i) serial_write(":");
        serial_write_hex(s_nic.mac[i]); /* prints 16 hex chars, trim later */
    }
    serial_write("\n");

    /* ---- Set up RX ring ---- */
    uint64_t rx_ring_phys = pmm_alloc_page();
    if (rx_ring_phys == PMM_ALLOC_FAILED) return false;
    s_rx_ring      = pv(rx_ring_phys);
    s_rx_ring_phys = rx_ring_phys;
    memset(s_rx_ring, 0, 4096);

    for (uint32_t i = 0; i < RX_RING_SIZE; ++i) {
        uint64_t p = pmm_alloc_page();
        if (p == PMM_ALLOC_FAILED) return false;
        s_rx_buf_phys[i] = p;
        s_rx_bufs[i]     = pv(p);
        s_rx_ring[i].addr   = p;
        s_rx_ring[i].status = 0;
    }
    s_rx_tail = 0;

    e_write(E1000_RDBAL, (uint32_t)(rx_ring_phys & 0xffffffffU));
    e_write(E1000_RDBAH, (uint32_t)(rx_ring_phys >> 32));
    e_write(E1000_RDLEN, RX_RING_SIZE * sizeof(rx_desc_t));
    e_write(E1000_RDH, 0);
    e_write(E1000_RDT, RX_RING_SIZE - 1);
    e_write(E1000_RCTL,
            RCTL_EN | RCTL_UPE | RCTL_MPE | RCTL_BAM |
            RCTL_BSIZE_2K | RCTL_SECRC);

    /* ---- Set up TX ring ---- */
    uint64_t tx_ring_phys = pmm_alloc_page();
    if (tx_ring_phys == PMM_ALLOC_FAILED) return false;
    s_tx_ring      = pv(tx_ring_phys);
    s_tx_ring_phys = tx_ring_phys;
    memset(s_tx_ring, 0, 4096);

    uint64_t tx_buf_phys = pmm_alloc_page();
    if (tx_buf_phys == PMM_ALLOC_FAILED) return false;
    s_tx_buf_phys = tx_buf_phys;
    s_tx_buf      = pv(tx_buf_phys);
    s_tx_tail     = 0;

    e_write(E1000_TDBAL, (uint32_t)(tx_ring_phys & 0xffffffffU));
    e_write(E1000_TDBAH, (uint32_t)(tx_ring_phys >> 32));
    e_write(E1000_TDLEN, TX_RING_SIZE * sizeof(tx_desc_t));
    e_write(E1000_TDH, 0);
    e_write(E1000_TDT, 0);
    e_write(E1000_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT_DEF | TCTL_COLD_DEF);
    e_write(E1000_TIPG, 0x0060200AU);

    s_nic.send = e1000_nic_send;
    s_nic.recv = e1000_nic_recv;
    nic_register(&s_nic);

    serial_write("e1000: ready\n");
    return true;
}
