#include "drivers/xhci.h"
#include "drivers/pci.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/serial.h"
#include "lib/string.h"
#include "mem/pmm.h"
#include "mem/vmm.h"

/* ============================================================================
 * xHCI host-controller driver (polling, single device, USB mass storage).
 *
 * Modeled on drivers/nvme.c: MMIO mapped at a fixed high virtual address, DMA
 * structures allocated from the PMM and reached through the direct map, ring
 * buffers with a cycle bit (the analog of NVMe's phase bit), and iteration-
 * count timeouts instead of interrupts.
 *
 * We enumerate exactly one device on the first connected root-hub port and
 * configure its Bulk-Only mass-storage endpoints. No hubs, no hot-plug.
 * ========================================================================== */

/* MMIO window: placed just above the NVMe window (0x…10000 + 32 KiB). 64 KiB
   comfortably covers capability + operational + runtime + doorbell regions. */
#define XHCI_MMIO_VIRT  0xffffa00000018000ULL
#define XHCI_MMIO_PAGES 16u

/* ---- Capability registers (offsets from MMIO base) ----------------------- */
#define XCAP_CAPLENGTH   0x00u   /* byte: length of cap regs */
#define XCAP_HCIVERSION  0x02u   /* word */
#define XCAP_HCSPARAMS1  0x04u
#define XCAP_HCSPARAMS2  0x08u
#define XCAP_HCCPARAMS1  0x10u
#define XCAP_DBOFF       0x14u
#define XCAP_RTSOFF      0x18u

/* ---- Operational registers (offsets from operational base) --------------- */
#define XOP_USBCMD   0x00u
#define XOP_USBSTS   0x04u
#define XOP_PAGESIZE 0x08u
#define XOP_DNCTRL   0x14u
#define XOP_CRCR     0x18u   /* 64-bit */
#define XOP_DCBAAP   0x30u   /* 64-bit */
#define XOP_CONFIG   0x38u
#define XOP_PORTS    0x400u  /* PORTSC array; port N at +((N-1)*0x10) */

#define USBCMD_RS    (1u << 0)
#define USBCMD_HCRST (1u << 1)
#define USBCMD_INTE  (1u << 2)

#define USBSTS_HCH   (1u << 0)   /* HC halted */
#define USBSTS_CNR   (1u << 11)  /* controller not ready */

/* PORTSC bits */
#define PORTSC_CCS   (1u << 0)   /* current connect status */
#define PORTSC_PED   (1u << 1)   /* port enabled/disabled */
#define PORTSC_PR    (1u << 4)   /* port reset */
#define PORTSC_PP    (1u << 9)   /* port power */
#define PORTSC_CSC   (1u << 17)  /* connect status change */
#define PORTSC_PRC   (1u << 21)  /* port reset change */
/* Bits that are write-1-to-clear; preserve the rest on read-modify-write. */
#define PORTSC_RW1CS (PORTSC_CSC | PORTSC_PRC | (1u<<18) | (1u<<19) | \
                      (1u<<20) | (1u<<22) | (1u<<23))
#define PORTSC_SPEED_SHIFT 10u

/* ---- Runtime / interrupter 0 (offsets from runtime base) ----------------- */
#define XRT_IR0      0x20u   /* interrupter 0 register set */
#define IR_IMAN      0x00u
#define IR_IMOD      0x04u
#define IR_ERSTSZ    0x08u
#define IR_ERSTBA    0x10u   /* 64-bit */
#define IR_ERDP      0x18u   /* 64-bit */
#define ERDP_EHB     (1u << 3)   /* event handler busy (write-1-clear) */

/* ---- TRB types ----------------------------------------------------------- */
#define TRB_NORMAL         1u
#define TRB_SETUP_STAGE    2u
#define TRB_DATA_STAGE     3u
#define TRB_STATUS_STAGE   4u
#define TRB_LINK           6u
#define TRB_ENABLE_SLOT    9u
#define TRB_ADDRESS_DEVICE 11u
#define TRB_CONFIGURE_EP   12u
#define TRB_EV_TRANSFER    32u
#define TRB_EV_CMD_COMPLETE 33u
#define TRB_EV_PORT_STATUS 34u

#define TRB_TYPE(t)   ((uint32_t)(t) << 10)
#define TRB_GET_TYPE(c) (((c) >> 10) & 0x3fu)
#define TRB_CYCLE     (1u << 0)
#define TRB_ENT       (1u << 1)   /* evaluate next TRB (chain for link) */
#define TRB_IOC       (1u << 5)   /* interrupt on completion */
#define TRB_IDT       (1u << 6)   /* immediate data (setup stage) */
#define TRB_CHAIN     (1u << 4)
/* completion code lives in event TRB status[31:24] */
#define TRB_CC(status) (((status) >> 24) & 0xffu)
#define CC_SUCCESS     1u
#define CC_SHORT_PKT   13u

/* Control transfer TRT (transfer type) field in Setup stage control dword. */
#define TRT_NO_DATA 0u
#define TRT_OUT     2u
#define TRT_IN      3u

/* Endpoint types (EP context) */
#define EPTYPE_BULK_OUT 2u
#define EPTYPE_CONTROL  4u
#define EPTYPE_BULK_IN  6u

/* ---- A 16-byte TRB ------------------------------------------------------- */
typedef struct {
    uint64_t param;
    uint32_t status;
    uint32_t control;
} __attribute__((packed)) trb_t;

#define RING_TRBS 64u   /* TRBs per ring (last is the Link TRB) */

/* ---- Driver state -------------------------------------------------------- */
static volatile uint8_t *s_cap;    /* capability register base */
static volatile uint8_t *s_op;     /* operational register base */
static volatile uint8_t *s_rt;     /* runtime register base */
static volatile uint32_t *s_db;    /* doorbell array */

static uint32_t s_ctx_size;        /* 32 or 64 (from HCCPARAMS1.CSZ) */
static uint32_t s_max_ports;

static uint64_t *s_dcbaa;          /* device context base address array */
static uint64_t  s_dcbaa_phys;

static trb_t   *s_cmd_ring;        /* command ring */
static uint64_t s_cmd_ring_phys;
static uint32_t s_cmd_enq;         /* enqueue index */
static uint8_t  s_cmd_cycle;

static trb_t   *s_evt_ring;        /* event ring */
static uint64_t s_evt_ring_phys;
static uint32_t s_evt_deq;         /* dequeue index */
static uint8_t  s_evt_cycle;
static uint64_t s_erst_phys;

static uint8_t  s_slot_id;
static uint8_t *s_dev_ctx;         /* output device context */
static uint64_t s_dev_ctx_phys;
static uint8_t *s_in_ctx;          /* input context (reused for cmds) */
static uint64_t s_in_ctx_phys;
static uint8_t  s_root_port;       /* 1-based */
static uint32_t s_port_speed;

/* transfer rings: EP0 (control) + bulk in + bulk out */
static trb_t   *s_ep0_ring;   static uint64_t s_ep0_ring_phys;   static uint32_t s_ep0_enq; static uint8_t s_ep0_cycle;
static trb_t   *s_bin_ring;   static uint64_t s_bin_ring_phys;   static uint32_t s_bin_enq; static uint8_t s_bin_cycle;
static trb_t   *s_bout_ring;  static uint64_t s_bout_ring_phys;  static uint32_t s_bout_enq; static uint8_t s_bout_cycle;
static uint8_t  s_bin_dci;    /* device context index of bulk-IN endpoint */
static uint8_t  s_bout_dci;
static uint16_t s_ep0_mps;    /* EP0 max packet size */

static bool s_msc_present;

/* ---- MMIO helpers -------------------------------------------------------- */
static uint32_t cap_rd32(uint32_t o){ return *(volatile uint32_t *)(s_cap + o); }
static uint32_t op_rd32(uint32_t o){ return *(volatile uint32_t *)(s_op + o); }
static void     op_wr32(uint32_t o, uint32_t v){ *(volatile uint32_t *)(s_op + o) = v; }
static void     op_wr64(uint32_t o, uint64_t v){
    *(volatile uint32_t *)(s_op + o)     = (uint32_t)v;
    *(volatile uint32_t *)(s_op + o + 4) = (uint32_t)(v >> 32);
}
static void     rt_wr32(uint32_t o, uint32_t v){ *(volatile uint32_t *)(s_rt + o) = v; }
static void     rt_wr64(uint32_t o, uint64_t v){
    *(volatile uint32_t *)(s_rt + o)     = (uint32_t)v;
    *(volatile uint32_t *)(s_rt + o + 4) = (uint32_t)(v >> 32);
}
static uint32_t portsc_rd(uint8_t port){ return op_rd32(XOP_PORTS + (uint32_t)(port-1)*0x10u); }
static void     portsc_wr(uint8_t port, uint32_t v){ op_wr32(XOP_PORTS + (uint32_t)(port-1)*0x10u, v); }

static void *dma_page(uint64_t *phys_out){
    uint64_t p = pmm_alloc_page();
    if (p == PMM_ALLOC_FAILED) return NULL;
    void *v = (void *)(uintptr_t)(vmm_direct_map_base() + p);
    memset(v, 0, 4096);
    if (phys_out) *phys_out = p;
    return v;
}

static void spin(uint32_t n){ for (volatile uint32_t i = 0; i < n * 1000u; ++i) {} }

/* ---- Ring init: last TRB is a Link back to the top ----------------------- */
static void ring_init(trb_t *ring, uint64_t ring_phys)
{
    memset(ring, 0, RING_TRBS * sizeof(trb_t));
    trb_t *link = &ring[RING_TRBS - 1];
    link->param   = ring_phys;
    link->status  = 0;
    /* Link TRB, Toggle Cycle so the producer cycle flips each wrap. */
    link->control = TRB_TYPE(TRB_LINK) | TRB_ENT | (1u << 1) /*TC*/ ;
}

/* Push a TRB onto a producer ring, handling the Link/cycle wrap. Returns the
   physical address the TRB was written at (for matching transfer events). */
static uint64_t ring_push(trb_t *ring, uint64_t ring_phys, uint32_t *enq,
                          uint8_t *cycle, uint64_t param, uint32_t status,
                          uint32_t control)
{
    trb_t *t = &ring[*enq];
    t->param  = param;
    t->status = status;
    /* set type/flags with current cycle */
    t->control = control | (*cycle ? TRB_CYCLE : 0u);
    uint64_t phys = ring_phys + (uint64_t)(*enq) * sizeof(trb_t);
    (*enq)++;
    if (*enq == RING_TRBS - 1) {
        /* hit the Link TRB: set its cycle so the HC follows it, then wrap. */
        trb_t *link = &ring[RING_TRBS - 1];
        link->control = (link->control & ~TRB_CYCLE) | (*cycle ? TRB_CYCLE : 0u);
        *enq = 0;
        *cycle ^= 1u;
    }
    return phys;
}

/* ---- Event ring: poll for the next event with the expected cycle --------- */
static bool evt_wait(trb_t *out, uint32_t iters)
{
    for (uint32_t i = 0; i < iters; ++i) {
        trb_t *e = &s_evt_ring[s_evt_deq];
        if ((e->control & TRB_CYCLE) == s_evt_cycle) {
            *out = *e;
            s_evt_deq++;
            if (s_evt_deq == RING_TRBS) { s_evt_deq = 0; s_evt_cycle ^= 1u; }
            /* advance ERDP (clear EHB) */
            rt_wr64(XRT_IR0 + IR_ERDP,
                    (s_evt_ring_phys + (uint64_t)s_evt_deq * sizeof(trb_t)) | ERDP_EHB);
            return true;
        }
        spin(2);
    }
    return false;
}

/* Ring the command doorbell and wait for its Command Completion Event.
   Returns the completion code, and (if slot_out) the slot id from the event. */
static uint32_t cmd_exec(uint64_t trb_phys, uint8_t *slot_out)
{
    s_db[0] = 0;  /* doorbell 0, target 0 = command ring */
    trb_t ev;
    for (uint32_t tries = 0; tries < 16; ++tries) {
        if (!evt_wait(&ev, 500000u)) return 0xff;
        if (TRB_GET_TYPE(ev.control) != TRB_EV_CMD_COMPLETE) continue;
        if (ev.param != trb_phys) continue;   /* not our command */
        if (slot_out) *slot_out = (uint8_t)(ev.control >> 24);
        return TRB_CC(ev.status);
    }
    return 0xff;
}

/* Ring an endpoint doorbell and wait for its Transfer Event. Returns completion
   code; SUCCESS or SHORT_PACKET are both "ok". */
static uint32_t xfer_wait(uint64_t last_trb_phys, uint32_t *residual)
{
    trb_t ev;
    for (uint32_t tries = 0; tries < 32; ++tries) {
        if (!evt_wait(&ev, 1000000u)) return 0xff;
        if (TRB_GET_TYPE(ev.control) != TRB_EV_TRANSFER) continue;
        /* Match the TRB pointer when the event carries one. */
        if (ev.param != last_trb_phys && ev.param != 0) continue;
        if (residual) *residual = ev.status & 0xffffffu;
        return TRB_CC(ev.status);
    }
    return 0xff;
}

/* ---- Context helpers (context stride is 32 or 64 bytes) ------------------ */
static uint32_t *slot_ctx(uint8_t *base){ return (uint32_t *)(base + s_ctx_size); }
static uint32_t *ep_ctx(uint8_t *base, uint8_t dci){ return (uint32_t *)(base + (uint32_t)(dci + 1) * s_ctx_size); }
/* input control context is the first block of the input context */
static uint32_t *in_ctrl_ctx(uint8_t *base){ return (uint32_t *)base; }

/* Max packet size for the default control endpoint by port speed. */
static uint16_t ep0_mps_for_speed(uint32_t speed)
{
    switch (speed) {
    case 1: return 8;    /* full speed (may be re-read from descriptor) */
    case 2: return 8;    /* low speed */
    case 3: return 64;   /* high speed */
    case 4: return 512;  /* super speed */
    default: return 64;
    }
}

/* ============================ bring-up =================================== */

static bool controller_reset(void)
{
    /* Halt if running. */
    uint32_t cmd = op_rd32(XOP_USBCMD);
    if (cmd & USBCMD_RS) {
        op_wr32(XOP_USBCMD, cmd & ~USBCMD_RS);
        for (uint32_t i = 0; i < 100000u; ++i) {
            if (op_rd32(XOP_USBSTS) & USBSTS_HCH) break;
            spin(1);
        }
    }
    /* Reset. */
    op_wr32(XOP_USBCMD, op_rd32(XOP_USBCMD) | USBCMD_HCRST);
    for (uint32_t i = 0; i < 1000000u; ++i) {
        if (!(op_rd32(XOP_USBCMD) & USBCMD_HCRST) &&
            !(op_rd32(XOP_USBSTS) & USBSTS_CNR))
            return true;
        spin(1);
    }
    return false;
}

/* Take ownership from firmware via the USB Legacy Support extended cap. */
static void bios_handoff(struct pci_addr addr, uint32_t hccparams1)
{
    uint32_t xecp = (hccparams1 >> 16) & 0xffffu;   /* in dwords from cap base */
    if (xecp == 0) return;
    volatile uint32_t *p = (volatile uint32_t *)(s_cap + xecp * 4u);
    for (uint32_t guard = 0; guard < 64u; ++guard) {
        uint32_t cap = p[0];
        uint8_t  id  = cap & 0xffu;
        if (id == 1u) {   /* USB Legacy Support */
            /* set OS Owned (bit 24); wait BIOS Owned (bit 16) to clear */
            p[0] = cap | (1u << 24);
            for (uint32_t i = 0; i < 100000u; ++i) {
                if (!(p[0] & (1u << 16))) break;
                spin(1);
            }
            /* disable SMIs in USBLEGCTLSTS (next dword) */
            p[1] = p[1] & ~0x0000e0ffu;
            (void)addr;
            return;
        }
        uint8_t next = (cap >> 8) & 0xffu;
        if (next == 0) break;
        p += next;   /* next pointer is in dwords */
    }
}

static bool setup_rings_and_contexts(void)
{
    /* DCBAA */
    s_dcbaa = dma_page(&s_dcbaa_phys);
    if (!s_dcbaa) return false;

    /* Scratchpad buffers, if the controller requires any. */
    uint32_t hcs2 = cap_rd32(XCAP_HCSPARAMS2);
    uint32_t spb = ((hcs2 >> 27) & 0x1fu) | (((hcs2 >> 21) & 0x1fu) << 5);
    if (spb) {
        uint64_t arr_phys;
        uint64_t *arr = dma_page(&arr_phys);
        if (!arr) return false;
        for (uint32_t i = 0; i < spb && i < 512u; ++i) {
            uint64_t bp;
            if (!dma_page(&bp)) return false;
            arr[i] = bp;
        }
        s_dcbaa[0] = arr_phys;
    }

    /* Command ring */
    s_cmd_ring = dma_page(&s_cmd_ring_phys);
    if (!s_cmd_ring) return false;
    ring_init(s_cmd_ring, s_cmd_ring_phys);
    s_cmd_enq = 0; s_cmd_cycle = 1;

    /* Event ring + ERST */
    s_evt_ring = dma_page(&s_evt_ring_phys);
    if (!s_evt_ring) return false;
    s_evt_deq = 0; s_evt_cycle = 1;
    uint32_t *erst = dma_page(&s_erst_phys);
    if (!erst) return false;
    erst[0] = (uint32_t)s_evt_ring_phys;
    erst[1] = (uint32_t)(s_evt_ring_phys >> 32);
    erst[2] = RING_TRBS;   /* segment size in TRBs */
    erst[3] = 0;

    /* Program operational registers. */
    op_wr32(XOP_CONFIG, cap_rd32(XCAP_HCSPARAMS1) & 0xffu);   /* MaxSlotsEn */
    op_wr64(XOP_DCBAAP, s_dcbaa_phys);
    op_wr64(XOP_CRCR, s_cmd_ring_phys | 1u /*RCS*/);

    rt_wr32(XRT_IR0 + IR_ERSTSZ, 1u);
    rt_wr64(XRT_IR0 + IR_ERDP, s_evt_ring_phys);
    rt_wr64(XRT_IR0 + IR_ERSTBA, s_erst_phys);
    return true;
}

static bool find_and_reset_port(void)
{
    for (uint8_t port = 1; port <= s_max_ports; ++port) {
        uint32_t sc = portsc_rd(port);
        if (!(sc & PORTSC_CCS)) continue;

        /* Power on if needed, then reset. Preserve non-RW1C bits. */
        uint32_t base = sc & ~PORTSC_RW1CS;
        portsc_wr(port, base | PORTSC_PR);
        for (uint32_t i = 0; i < 500000u; ++i) {
            uint32_t v = portsc_rd(port);
            if (v & PORTSC_PRC) {   /* reset complete */
                /* clear change bits (write-1-clear) */
                portsc_wr(port, (v & ~PORTSC_RW1CS) | PORTSC_PRC | PORTSC_CSC);
                if (v & PORTSC_PED) {
                    s_root_port  = port;
                    s_port_speed = (v >> PORTSC_SPEED_SHIFT) & 0xfu;
                    return true;
                }
                break;
            }
            spin(1);
        }
    }
    return false;
}

/* Build the input context for Address Device (A0=slot, A1=EP0). */
static bool address_device(void)
{
    /* device output context + DCBAA slot entry */
    s_dev_ctx = dma_page(&s_dev_ctx_phys);
    if (!s_dev_ctx) return false;
    s_dcbaa[s_slot_id] = s_dev_ctx_phys;

    /* EP0 transfer ring */
    s_ep0_ring = dma_page(&s_ep0_ring_phys);
    if (!s_ep0_ring) return false;
    ring_init(s_ep0_ring, s_ep0_ring_phys);
    s_ep0_enq = 0; s_ep0_cycle = 1;

    s_in_ctx = dma_page(&s_in_ctx_phys);
    if (!s_in_ctx) return false;

    uint32_t *ic = in_ctrl_ctx(s_in_ctx);
    ic[1] = (1u << 0) | (1u << 1);   /* Add slot + EP0 */

    uint32_t *sc = slot_ctx(s_in_ctx);
    /* dword0: route string (0) | speed[23:20] | context entries[31:27]=1 */
    sc[0] = ((uint32_t)s_port_speed << 20) | (1u << 27);
    /* dword1: root hub port number[23:16] */
    sc[1] = ((uint32_t)s_root_port << 16);

    s_ep0_mps = ep0_mps_for_speed(s_port_speed);
    uint32_t *ep0 = ep_ctx(s_in_ctx, 1);   /* DCI 1 = default control EP0 */
    /* dword1: EP type[5:3]=control(4), max packet size[31:16], CErr[2:1]=3 */
    ep0[1] = (EPTYPE_CONTROL << 3) | (3u << 1) | ((uint32_t)s_ep0_mps << 16);
    /* dword2/3: TR dequeue ptr | DCS(bit0)=1 */
    ep0[2] = (uint32_t)(s_ep0_ring_phys | 1u);
    ep0[3] = (uint32_t)(s_ep0_ring_phys >> 32);

    uint64_t p = ring_push(s_cmd_ring, s_cmd_ring_phys, &s_cmd_enq, &s_cmd_cycle,
                           s_in_ctx_phys, 0,
                           TRB_TYPE(TRB_ADDRESS_DEVICE) |
                           ((uint32_t)s_slot_id << 24));
    return cmd_exec(p, NULL) == CC_SUCCESS;
}

/* ---- Control transfer (used for descriptors + class requests) ------------ */
int xhci_control(uint8_t bm_request_type, uint8_t b_request,
                 uint16_t w_value, uint16_t w_index, uint16_t w_length,
                 uint64_t buf_phys)
{
    bool in = (bm_request_type & 0x80u) != 0;
    uint32_t trt = (w_length == 0) ? TRT_NO_DATA : (in ? TRT_IN : TRT_OUT);

    /* Setup stage: 8 bytes of the setup packet packed as immediate data. */
    uint64_t setup = (uint64_t)bm_request_type | ((uint64_t)b_request << 8) |
                     ((uint64_t)w_value << 16) | ((uint64_t)w_index << 32) |
                     ((uint64_t)w_length << 48);
    ring_push(s_ep0_ring, s_ep0_ring_phys, &s_ep0_enq, &s_ep0_cycle,
              setup, 8u,
              TRB_TYPE(TRB_SETUP_STAGE) | TRB_IDT | ((uint32_t)trt << 16));

    uint64_t data_phys = 0;
    if (w_length) {
        data_phys = ring_push(s_ep0_ring, s_ep0_ring_phys, &s_ep0_enq, &s_ep0_cycle,
                              buf_phys, w_length,
                              TRB_TYPE(TRB_DATA_STAGE) | (in ? (1u << 16) : 0u));
    }

    /* Status stage: opposite direction, IOC so we get an event. */
    uint64_t st = ring_push(s_ep0_ring, s_ep0_ring_phys, &s_ep0_enq, &s_ep0_cycle,
                            0, 0,
                            TRB_TYPE(TRB_STATUS_STAGE) | TRB_IOC |
                            ((in || w_length == 0) ? 0u : (1u << 16)));

    s_db[s_slot_id] = 1;   /* EP0 = DCI 1 */
    (void)data_phys;
    uint32_t cc = xfer_wait(st, NULL);
    return (cc == CC_SUCCESS || cc == CC_SHORT_PKT) ? 0 : -1;
}

/* ---- Descriptor parsing + endpoint configuration ------------------------- */
static bool enumerate_msc(void)
{
    uint64_t buf_phys;
    uint8_t *buf = dma_page(&buf_phys);
    if (!buf) return false;

    /* GET_DESCRIPTOR(device, 18) */
    if (xhci_control(0x80, 0x06, 0x0100, 0, 18, buf_phys) != 0) return false;
    if (buf[7]) s_ep0_mps = buf[7];   /* bMaxPacketSize0 (byte for LS/FS/HS) */

    /* GET_DESCRIPTOR(config, 9) then full length. */
    if (xhci_control(0x80, 0x06, 0x0200, 0, 9, buf_phys) != 0) return false;
    uint16_t total = (uint16_t)(buf[2] | (buf[3] << 8));
    if (total > 4096) total = 4096;
    if (xhci_control(0x80, 0x06, 0x0200, 0, total, buf_phys) != 0) return false;

    uint8_t cfg_value = buf[5];
    /* Walk interface/endpoint descriptors looking for a BOT MSC interface. */
    bool found_if = false;
    uint8_t bulk_in_addr = 0, bulk_out_addr = 0;
    uint16_t bin_mps = 512, bout_mps = 512;
    uint32_t i = 0;
    while (i + 2 <= total) {
        uint8_t len = buf[i];
        uint8_t type = buf[i + 1];
        if (len == 0) break;
        if (type == 0x04 && i + 9 <= total) {           /* INTERFACE */
            uint8_t iclass = buf[i + 5], isub = buf[i + 6], iproto = buf[i + 7];
            found_if = (iclass == 0x08 && iproto == 0x50); /* MSC, Bulk-Only */
            (void)isub;
        } else if (type == 0x05 && found_if && i + 7 <= total) { /* ENDPOINT */
            uint8_t eaddr = buf[i + 2];
            uint8_t eattr = buf[i + 3] & 0x03u;
            uint16_t emps = (uint16_t)(buf[i + 4] | (buf[i + 5] << 8));
            if (eattr == 0x02) {                          /* bulk */
                if (eaddr & 0x80u) { bulk_in_addr = eaddr; bin_mps = emps; }
                else               { bulk_out_addr = eaddr; bout_mps = emps; }
            }
        }
        i += len;
    }
    if (!bulk_in_addr || !bulk_out_addr) return false;

    /* SET_CONFIGURATION(cfg_value) */
    if (xhci_control(0x00, 0x09, cfg_value, 0, 0, 0) != 0) return false;

    /* DCI: EPn IN = 2n+1, EPn OUT = 2n, where n = endpoint number. */
    uint8_t in_num  = bulk_in_addr & 0x0fu;
    uint8_t out_num = bulk_out_addr & 0x0fu;
    s_bin_dci  = (uint8_t)(2u * in_num + 1u);
    s_bout_dci = (uint8_t)(2u * out_num);

    s_bin_ring = dma_page(&s_bin_ring_phys);
    s_bout_ring = dma_page(&s_bout_ring_phys);
    if (!s_bin_ring || !s_bout_ring) return false;
    ring_init(s_bin_ring, s_bin_ring_phys);   s_bin_enq = 0;  s_bin_cycle = 1;
    ring_init(s_bout_ring, s_bout_ring_phys);  s_bout_enq = 0; s_bout_cycle = 1;

    /* Configure Endpoint: A0(slot) + the two bulk endpoints. */
    memset(s_in_ctx, 0, 4096);
    uint32_t *ic = in_ctrl_ctx(s_in_ctx);
    ic[1] = (1u << 0) | (1u << s_bin_dci) | (1u << s_bout_dci);

    uint32_t *sc = slot_ctx(s_in_ctx);
    uint8_t max_dci = s_bin_dci > s_bout_dci ? s_bin_dci : s_bout_dci;
    sc[0] = ((uint32_t)s_port_speed << 20) | ((uint32_t)max_dci << 27);
    sc[1] = ((uint32_t)s_root_port << 16);

    uint32_t *ein = ep_ctx(s_in_ctx, s_bin_dci);
    ein[1] = (EPTYPE_BULK_IN << 3) | (3u << 1) | ((uint32_t)bin_mps << 16);
    ein[2] = (uint32_t)(s_bin_ring_phys | 1u);
    ein[3] = (uint32_t)(s_bin_ring_phys >> 32);

    uint32_t *eout = ep_ctx(s_in_ctx, s_bout_dci);
    eout[1] = (EPTYPE_BULK_OUT << 3) | (3u << 1) | ((uint32_t)bout_mps << 16);
    eout[2] = (uint32_t)(s_bout_ring_phys | 1u);
    eout[3] = (uint32_t)(s_bout_ring_phys >> 32);

    uint64_t p = ring_push(s_cmd_ring, s_cmd_ring_phys, &s_cmd_enq, &s_cmd_cycle,
                           s_in_ctx_phys, 0,
                           TRB_TYPE(TRB_CONFIGURE_EP) |
                           ((uint32_t)s_slot_id << 24));
    if (cmd_exec(p, NULL) != CC_SUCCESS) return false;

    serial_write("xhci: MSC endpoints configured (bin dci=");
    serial_write_dec(s_bin_dci);
    serial_write(" bout dci=");
    serial_write_dec(s_bout_dci);
    serial_write(")\n");
    return true;
}

/* ---- Bulk transfers ------------------------------------------------------ */
static int bulk_xfer(bool in, uint64_t buf_phys, uint32_t len)
{
    trb_t *ring   = in ? s_bin_ring : s_bout_ring;
    uint64_t rp   = in ? s_bin_ring_phys : s_bout_ring_phys;
    uint32_t *enq = in ? &s_bin_enq : &s_bout_enq;
    uint8_t  *cyc = in ? &s_bin_cycle : &s_bout_cycle;
    uint8_t   dci = in ? s_bin_dci : s_bout_dci;

    uint64_t p = ring_push(ring, rp, enq, cyc, buf_phys, len,
                           TRB_TYPE(TRB_NORMAL) | TRB_IOC);
    s_db[s_slot_id] = dci;
    uint32_t residual = 0;
    uint32_t cc = xfer_wait(p, &residual);
    if (cc != CC_SUCCESS && cc != CC_SHORT_PKT) return -1;
    return (int)(len - residual);
}

int xhci_bulk_out(uint64_t buf_phys, uint32_t len){ return bulk_xfer(false, buf_phys, len); }
int xhci_bulk_in(uint64_t buf_phys, uint32_t len){ return bulk_xfer(true, buf_phys, len); }

bool xhci_msc_present(void){ return s_msc_present; }

/* ============================ init ====================================== */
bool xhci_init(void)
{
    struct pci_addr addr = pci_find_class(0x0Cu, 0x03u);   /* serial bus / USB */
    if (!addr.valid) return false;
    uint32_t cc = pci_read32(addr.bus, addr.dev, addr.func, 0x08);
    if (((cc >> 8) & 0xffu) != 0x30u) {   /* prog-IF 0x30 = xHCI */
        serial_write("xhci: USB controller is not xHCI, skipping\n");
        return false;
    }

    serial_write("xhci: found at ");
    serial_write_dec(addr.bus); serial_write(":");
    serial_write_dec(addr.dev); serial_write(".");
    serial_write_dec(addr.func); serial_write("\n");

    pci_enable_busmaster(addr);
    uint64_t bar = pci_read_bar(addr, 0);
    if (bar == 0) { serial_write("xhci: BAR0 not mapped\n"); return false; }

    for (uint32_t pg = 0; pg < XHCI_MMIO_PAGES; ++pg)
        vmm_map(XHCI_MMIO_VIRT + (uint64_t)pg * 4096u,
                (bar & ~0xfffULL) + (uint64_t)pg * 4096u,
                VMM_WRITABLE | 0x010ULL /* PCD uncacheable */);
    s_cap = (volatile uint8_t *)(uintptr_t)XHCI_MMIO_VIRT;

    uint8_t  caplen = *(volatile uint8_t *)(s_cap + XCAP_CAPLENGTH);
    uint32_t hcs1   = cap_rd32(XCAP_HCSPARAMS1);
    uint32_t hcc1   = cap_rd32(XCAP_HCCPARAMS1);
    uint32_t dboff  = cap_rd32(XCAP_DBOFF) & ~0x3u;
    uint32_t rtsoff = cap_rd32(XCAP_RTSOFF) & ~0x1fu;

    s_op = s_cap + caplen;
    s_rt = s_cap + rtsoff;
    s_db = (volatile uint32_t *)(s_cap + dboff);
    s_ctx_size = (hcc1 & (1u << 2)) ? 64u : 32u;
    s_max_ports = (hcs1 >> 24) & 0xffu;

    serial_write("xhci: caplen="); serial_write_dec(caplen);
    serial_write(" ports="); serial_write_dec(s_max_ports);
    serial_write(" ctxsz="); serial_write_dec(s_ctx_size);
    serial_write("\n");

    bios_handoff(addr, hcc1);

    if (!controller_reset()) { serial_write("xhci: reset timeout\n"); return false; }
    if (!setup_rings_and_contexts()) { serial_write("xhci: ring setup failed\n"); return false; }

    /* Run. */
    op_wr32(XOP_USBCMD, op_rd32(XOP_USBCMD) | USBCMD_RS);
    spin(20);

    if (!find_and_reset_port()) { serial_write("xhci: no device on any port\n"); return false; }
    serial_write("xhci: port "); serial_write_dec(s_root_port);
    serial_write(" device connected (speed="); serial_write_dec(s_port_speed);
    serial_write(")\n");

    /* Enable Slot. */
    uint64_t p = ring_push(s_cmd_ring, s_cmd_ring_phys, &s_cmd_enq, &s_cmd_cycle,
                           0, 0, TRB_TYPE(TRB_ENABLE_SLOT));
    if (cmd_exec(p, &s_slot_id) != CC_SUCCESS || s_slot_id == 0) {
        serial_write("xhci: enable slot failed\n"); return false;
    }

    if (!address_device()) { serial_write("xhci: address device failed\n"); return false; }
    serial_write("xhci: device addressed (slot "); serial_write_dec(s_slot_id);
    serial_write(")\n");

    if (!enumerate_msc()) { serial_write("xhci: not a Bulk-Only MSC device\n"); return false; }

    s_msc_present = true;
    serial_write("xhci: ready\n");
    return true;
}
