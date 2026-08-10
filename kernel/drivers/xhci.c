#include "drivers/xhci.h"
#include "drivers/pci.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/serial.h"
#include "lib/string.h"
#include "mem/pmm.h"
#include "mem/vmm.h"
#include "sync/spinlock.h"

/* ============================================================================
 * xHCI host-controller driver (polling, multi-device).
 *
 * Enumerates every device on a connected root-hub port and configures each
 * device's endpoints (bulk + interrupt), so a mass-storage stick and a HID
 * keyboard can coexist. All transactions share one event ring; a single-reaper
 * + completion table (keyed by TRB physaddr, under xhci_lock) lets concurrent
 * transfers on different endpoints complete without losing each other's events.
 * No hubs, no hot-plug, no UAS.
 * ========================================================================== */

#define XHCI_MMIO_VIRT  0xffffa00000018000ULL
#define XHCI_MMIO_PAGES 16u

#define MAX_DEV     4
#define MAX_EP      8
#define RING_TRBS   64u
#define MAX_PENDING 16

/* ---- Capability / operational / runtime register offsets ----------------- */
#define XCAP_CAPLENGTH   0x00u
#define XCAP_HCSPARAMS1  0x04u
#define XCAP_HCSPARAMS2  0x08u
#define XCAP_HCCPARAMS1  0x10u
#define XCAP_DBOFF       0x14u
#define XCAP_RTSOFF      0x18u

#define XOP_USBCMD   0x00u
#define XOP_USBSTS   0x04u
#define XOP_CRCR     0x18u
#define XOP_DCBAAP   0x30u
#define XOP_CONFIG   0x38u
#define XOP_PORTS    0x400u

#define USBCMD_RS    (1u << 0)
#define USBCMD_HCRST (1u << 1)
#define USBSTS_HCH   (1u << 0)
#define USBSTS_CNR   (1u << 11)

#define PORTSC_CCS   (1u << 0)
#define PORTSC_PED   (1u << 1)
#define PORTSC_PR    (1u << 4)
#define PORTSC_CSC   (1u << 17)
#define PORTSC_PRC   (1u << 21)
#define PORTSC_RW1CS (PORTSC_CSC | PORTSC_PRC | (1u<<18) | (1u<<19) | \
                      (1u<<20) | (1u<<22) | (1u<<23))
#define PORTSC_SPEED_SHIFT 10u

#define XRT_IR0   0x20u
#define IR_ERSTSZ 0x08u
#define IR_ERSTBA 0x10u
#define IR_ERDP   0x18u
#define ERDP_EHB  (1u << 3)

/* ---- TRB types ----------------------------------------------------------- */
#define TRB_NORMAL          1u
#define TRB_SETUP_STAGE     2u
#define TRB_DATA_STAGE      3u
#define TRB_STATUS_STAGE    4u
#define TRB_LINK            6u
#define TRB_ENABLE_SLOT     9u
#define TRB_ADDRESS_DEVICE  11u
#define TRB_CONFIGURE_EP    12u
#define TRB_EV_TRANSFER     32u
#define TRB_EV_CMD_COMPLETE 33u
#define TRB_EV_PORT_STATUS  34u

#define TRB_TYPE(t)     ((uint32_t)(t) << 10)
#define TRB_GET_TYPE(c) (((c) >> 10) & 0x3fu)
#define TRB_CYCLE       (1u << 0)
#define TRB_ENT         (1u << 1)
#define TRB_IOC         (1u << 5)
#define TRB_IDT         (1u << 6)
#define TRB_CC(status)  (((status) >> 24) & 0xffu)
#define CC_SUCCESS      1u
#define CC_SHORT_PKT    13u

#define TRT_NO_DATA 0u
#define TRT_OUT     2u
#define TRT_IN      3u

#define EPTYPE_ISOCH_OUT  1u
#define EPTYPE_BULK_OUT   2u
#define EPTYPE_INTR_OUT   3u
#define EPTYPE_CONTROL    4u
#define EPTYPE_BULK_IN    6u
#define EPTYPE_INTR_IN    7u

typedef struct {
    uint64_t param;
    uint32_t status;
    uint32_t control;
} __attribute__((packed)) trb_t;

/* ---- Per-endpoint / per-device state ------------------------------------- */
struct ep {
    uint8_t  dci;
    uint8_t  type;
    trb_t   *ring;
    uint64_t ring_phys;
    uint32_t enq;
    uint8_t  cycle;
    int      pending;   /* completion index of an outstanding TRB, or -1 */
};

struct xhci_dev {
    bool     used;
    uint8_t  slot_id;
    uint8_t  root_port;
    uint32_t speed;
    uint8_t *dev_ctx;  uint64_t dev_ctx_phys;
    uint8_t *in_ctx;   uint64_t in_ctx_phys;
    struct ep ep0;     uint16_t ep0_mps;
    struct ep eps[MAX_EP]; int nep;
    uint8_t  iclass, isub, iproto;
    int      bulk_in, bulk_out, intr_in;  /* indices into eps[], -1 if absent */
};

/* ---- Completion table (single reaper) ------------------------------------ */
struct completion {
    bool     used;
    bool     done;
    uint64_t trb_phys;
    uint32_t cc;
    uint32_t residue;
    uint8_t  slot;      /* slot id from a command-completion event */
};

/* ---- Controller state ---------------------------------------------------- */
static volatile uint8_t  *s_cap;
static volatile uint8_t  *s_op;
static volatile uint8_t  *s_rt;
static volatile uint32_t *s_db;
static uint32_t s_ctx_size;
static uint32_t s_max_ports;

static uint64_t *s_dcbaa;      static uint64_t s_dcbaa_phys;
static trb_t    *s_cmd_ring;   static uint64_t s_cmd_ring_phys;
static uint32_t  s_cmd_enq;    static uint8_t  s_cmd_cycle;
/* Controller-written DMA: volatile so the cycle-bit load below cannot be
   hoisted.  Today the spinlock's memory clobber inside comp_wait's poll
   loop also forces the reload, but that is a property of where the lock
   happens to sit, not of the ring — don't depend on it. */
static volatile trb_t *s_evt_ring;   static uint64_t s_evt_ring_phys;
static uint32_t  s_evt_deq;    static uint8_t  s_evt_cycle;
static uint64_t  s_erst_phys;

static struct xhci_dev s_dev[MAX_DEV];
static int s_ndev;

static struct completion s_comp[MAX_PENDING];
static spinlock_t s_xlock = SPINLOCK_INIT;

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
static uint32_t portsc_rd(uint8_t p){ return op_rd32(XOP_PORTS + (uint32_t)(p-1)*0x10u); }
static void     portsc_wr(uint8_t p, uint32_t v){ op_wr32(XOP_PORTS + (uint32_t)(p-1)*0x10u, v); }

static void *dma_page(uint64_t *phys_out){
    uint64_t p = pmm_alloc_page();
    if (p == PMM_ALLOC_FAILED) return NULL;
    void *v = (void *)(uintptr_t)(vmm_direct_map_base() + p);
    memset(v, 0, 4096);
    if (phys_out) *phys_out = p;
    return v;
}
static void spin(uint32_t n){ for (volatile uint32_t i = 0; i < n * 1000u; ++i) {} }

/* ---- Rings --------------------------------------------------------------- */
static void ring_init(trb_t *ring, uint64_t ring_phys)
{
    memset(ring, 0, RING_TRBS * sizeof(trb_t));
    trb_t *link = &ring[RING_TRBS - 1];
    link->param   = ring_phys;
    link->control = TRB_TYPE(TRB_LINK) | TRB_ENT | (1u << 1) /*Toggle Cycle*/;
}

static uint64_t ring_push(trb_t *ring, uint64_t ring_phys, uint32_t *enq,
                          uint8_t *cycle, uint64_t param, uint32_t status,
                          uint32_t control)
{
    trb_t *t = &ring[*enq];
    t->param  = param;
    t->status = status;
    t->control = control | (*cycle ? TRB_CYCLE : 0u);
    uint64_t phys = ring_phys + (uint64_t)(*enq) * sizeof(trb_t);
    (*enq)++;
    if (*enq == RING_TRBS - 1) {
        trb_t *link = &ring[RING_TRBS - 1];
        link->control = (link->control & ~TRB_CYCLE) | (*cycle ? TRB_CYCLE : 0u);
        *enq = 0;
        *cycle ^= 1u;
    }
    return phys;
}

/* ---- Completion table + event reaper (caller holds s_xlock) -------------- */
static int comp_register(uint64_t trb_phys)
{
    for (int i = 0; i < MAX_PENDING; ++i) {
        if (!s_comp[i].used) {
            s_comp[i].used = true; s_comp[i].done = false;
            s_comp[i].trb_phys = trb_phys; s_comp[i].cc = 0; s_comp[i].residue = 0;
            return i;
        }
    }
    return -1;
}

/* Drain all currently-available events, filing completions by TRB physaddr. */
static void reap_events(void)
{
    for (;;) {
        volatile trb_t *e = &s_evt_ring[s_evt_deq];
        if ((e->control & TRB_CYCLE) != s_evt_cycle) break;

        uint32_t type = TRB_GET_TYPE(e->control);
        if (type == TRB_EV_TRANSFER || type == TRB_EV_CMD_COMPLETE) {
            for (int i = 0; i < MAX_PENDING; ++i) {
                if (s_comp[i].used && !s_comp[i].done &&
                    s_comp[i].trb_phys == e->param) {
                    s_comp[i].done = true;
                    s_comp[i].cc = TRB_CC(e->status);
                    s_comp[i].residue = e->status & 0xffffffu;
                    s_comp[i].slot = (uint8_t)(e->control >> 24);
                    break;
                }
            }
        }
        /* port status change and unmatched events are simply drained */
        s_evt_deq++;
        if (s_evt_deq == RING_TRBS) { s_evt_deq = 0; s_evt_cycle ^= 1u; }
        rt_wr64(XRT_IR0 + IR_ERDP,
                (s_evt_ring_phys + (uint64_t)s_evt_deq * sizeof(trb_t)) | ERDP_EHB);
    }
}

/* Wait for a registered completion; returns cc (0xff on timeout). Frees slot.
   Copies out residue and command-completion slot id before freeing. */
static uint32_t comp_wait(int idx, uint32_t *residue, uint8_t *slot, uint32_t iters)
{
    uint32_t cc = 0xff;
    for (uint32_t i = 0; i < iters; ++i) {
        uint64_t f = spinlock_acquire_irqsave(&s_xlock);
        reap_events();
        bool done = s_comp[idx].done;
        if (done) {
            cc = s_comp[idx].cc;
            if (residue) *residue = s_comp[idx].residue;
            if (slot) *slot = s_comp[idx].slot;
        }
        spinlock_release_irqrestore(&s_xlock, f);
        if (done) break;
        spin(2);
    }
    uint64_t f = spinlock_acquire_irqsave(&s_xlock);
    s_comp[idx].used = false;
    spinlock_release_irqrestore(&s_xlock, f);
    return cc;
}

/* ---- Command ring -------------------------------------------------------- */
static uint32_t cmd_exec(uint64_t param, uint32_t control, uint8_t *slot_out)
{
    uint64_t f = spinlock_acquire_irqsave(&s_xlock);
    uint64_t p = ring_push(s_cmd_ring, s_cmd_ring_phys, &s_cmd_enq, &s_cmd_cycle,
                           param, 0, control);
    int idx = comp_register(p);
    __asm__ volatile("" ::: "memory");   /* publish the TRB first */
    s_db[0] = 0;   /* command doorbell */
    spinlock_release_irqrestore(&s_xlock, f);
    if (idx < 0) return 0xff;

    /* The reaper captures the slot id (from the event's control field) into the
       completion entry; comp_wait copies it out for us (Enable Slot needs it). */
    return comp_wait(idx, NULL, slot_out, 500000u);
}

/* ---- Generic transfer: push TRB(s), doorbell, wait ----------------------- */
static uint32_t ep_transfer(struct xhci_dev *d, struct ep *e,
                            uint64_t buf_phys, uint32_t len, uint32_t *residue)
{
    uint64_t f = spinlock_acquire_irqsave(&s_xlock);
    uint64_t p = ring_push(e->ring, e->ring_phys, &e->enq, &e->cycle,
                           buf_phys, len, TRB_TYPE(TRB_NORMAL) | TRB_IOC);
    int idx = comp_register(p);
    __asm__ volatile("" ::: "memory");
    s_db[d->slot_id] = e->dci;
    spinlock_release_irqrestore(&s_xlock, f);
    if (idx < 0) return 0xff;
    return comp_wait(idx, residue, NULL, 1000000u);
}

/* ---- Public device lookup + transfers ------------------------------------ */
int xhci_find_device(uint8_t iclass, uint8_t isubclass, uint8_t iproto)
{
    for (int i = 0; i < s_ndev; ++i)
        if (s_dev[i].used && s_dev[i].iclass == iclass &&
            s_dev[i].isub == isubclass && s_dev[i].iproto == iproto)
            return i;
    return -1;
}

int xhci_control(int dev, uint8_t bm_request_type, uint8_t b_request,
                 uint16_t w_value, uint16_t w_index, uint16_t w_length,
                 uint64_t buf_phys)
{
    if (dev < 0 || dev >= MAX_DEV || !s_dev[dev].used) return -1;
    struct xhci_dev *d = &s_dev[dev];
    bool in = (bm_request_type & 0x80u) != 0;
    uint32_t trt = (w_length == 0) ? TRT_NO_DATA : (in ? TRT_IN : TRT_OUT);
    uint64_t setup = (uint64_t)bm_request_type | ((uint64_t)b_request << 8) |
                     ((uint64_t)w_value << 16) | ((uint64_t)w_index << 32) |
                     ((uint64_t)w_length << 48);

    uint64_t f = spinlock_acquire_irqsave(&s_xlock);
    ring_push(d->ep0.ring, d->ep0.ring_phys, &d->ep0.enq, &d->ep0.cycle,
              setup, 8u, TRB_TYPE(TRB_SETUP_STAGE) | TRB_IDT | ((uint32_t)trt << 16));
    if (w_length)
        ring_push(d->ep0.ring, d->ep0.ring_phys, &d->ep0.enq, &d->ep0.cycle,
                  buf_phys, w_length, TRB_TYPE(TRB_DATA_STAGE) | (in ? (1u<<16) : 0u));
    uint64_t stp = ring_push(d->ep0.ring, d->ep0.ring_phys, &d->ep0.enq, &d->ep0.cycle,
                             0, 0, TRB_TYPE(TRB_STATUS_STAGE) | TRB_IOC |
                             ((in || w_length == 0) ? 0u : (1u << 16)));
    int idx = comp_register(stp);
    __asm__ volatile("" ::: "memory");
    s_db[d->slot_id] = 1;   /* EP0 */
    spinlock_release_irqrestore(&s_xlock, f);
    if (idx < 0) return -1;

    uint32_t cc = comp_wait(idx, NULL, NULL, 1000000u);
    return (cc == CC_SUCCESS || cc == CC_SHORT_PKT) ? 0 : -1;
}

static int ep_io(int dev, int ep_index, uint64_t buf_phys, uint32_t len)
{
    if (dev < 0 || dev >= MAX_DEV || !s_dev[dev].used || ep_index < 0) return -1;
    struct xhci_dev *d = &s_dev[dev];
    uint32_t residue = 0;
    uint32_t cc = ep_transfer(d, &d->eps[ep_index], buf_phys, len, &residue);
    if (cc != CC_SUCCESS && cc != CC_SHORT_PKT) return -1;
    return (int)(len - residue);
}

int xhci_bulk_out(int dev, uint64_t buf_phys, uint32_t len){ return ep_io(dev, s_dev[dev].bulk_out, buf_phys, len); }
int xhci_bulk_in(int dev, uint64_t buf_phys, uint32_t len){ return ep_io(dev, s_dev[dev].bulk_in, buf_phys, len); }

/* Non-blocking interrupt-IN poll with a single outstanding TRB. Returns bytes
   on a completed report, -2 if still pending (call again later), -1 on error.
   Keeping one TRB armed (rather than re-arming each call) avoids accumulating
   pending TRBs on an endpoint that NAKs while idle (a keyboard between keys). */
int xhci_intr_poll(int dev, uint64_t buf_phys, uint32_t len)
{
    if (dev < 0 || dev >= MAX_DEV || !s_dev[dev].used) return -1;
    struct xhci_dev *d = &s_dev[dev];
    if (d->intr_in < 0) return -1;
    struct ep *e = &d->eps[d->intr_in];

    uint64_t f = spinlock_acquire_irqsave(&s_xlock);
    if (e->pending < 0) {   /* arm one TRB */
        uint64_t p = ring_push(e->ring, e->ring_phys, &e->enq, &e->cycle,
                               buf_phys, len, TRB_TYPE(TRB_NORMAL) | TRB_IOC);
        e->pending = comp_register(p);
        if (e->pending >= 0) { __asm__ volatile("" ::: "memory");
                               s_db[d->slot_id] = e->dci; }
    }
    reap_events();
    int idx = e->pending, ret = -2;
    if (idx >= 0 && s_comp[idx].done) {
        uint32_t cc = s_comp[idx].cc, res = s_comp[idx].residue;
        s_comp[idx].used = false;
        e->pending = -1;
        ret = (cc == CC_SUCCESS || cc == CC_SHORT_PKT) ? (int)(len - res) : -1;
    }
    spinlock_release_irqrestore(&s_xlock, f);
    return ret;
}

/* ---- Context helpers ----------------------------------------------------- */
static uint32_t *slot_ctx(uint8_t *base){ return (uint32_t *)(base + s_ctx_size); }
static uint32_t *ep_ctx(uint8_t *base, uint8_t dci){ return (uint32_t *)(base + (uint32_t)(dci + 1) * s_ctx_size); }
static uint32_t *in_ctrl_ctx(uint8_t *base){ return (uint32_t *)base; }

static uint16_t ep0_mps_for_speed(uint32_t speed)
{
    switch (speed) {
    case 1: return 64;   /* full speed */
    case 2: return 8;    /* low speed */
    case 3: return 64;   /* high speed */
    case 4: return 512;  /* super speed */
    default: return 64;
    }
}

/* ============================ bring-up =================================== */
static bool controller_reset(void)
{
    uint32_t cmd = op_rd32(XOP_USBCMD);
    if (cmd & USBCMD_RS) {
        op_wr32(XOP_USBCMD, cmd & ~USBCMD_RS);
        for (uint32_t i = 0; i < 100000u; ++i) {
            if (op_rd32(XOP_USBSTS) & USBSTS_HCH) break;
            spin(1);
        }
    }
    op_wr32(XOP_USBCMD, op_rd32(XOP_USBCMD) | USBCMD_HCRST);
    for (uint32_t i = 0; i < 1000000u; ++i) {
        if (!(op_rd32(XOP_USBCMD) & USBCMD_HCRST) &&
            !(op_rd32(XOP_USBSTS) & USBSTS_CNR))
            return true;
        spin(1);
    }
    return false;
}

static void bios_handoff(uint32_t hccparams1)
{
    uint32_t xecp = (hccparams1 >> 16) & 0xffffu;
    if (xecp == 0) return;
    volatile uint32_t *p = (volatile uint32_t *)(s_cap + xecp * 4u);
    for (uint32_t guard = 0; guard < 64u; ++guard) {
        uint32_t cap = p[0];
        if ((cap & 0xffu) == 1u) {   /* USB Legacy Support */
            p[0] = cap | (1u << 24);
            for (uint32_t i = 0; i < 100000u; ++i) {
                if (!(p[0] & (1u << 16))) break;
                spin(1);
            }
            p[1] = p[1] & ~0x0000e0ffu;
            return;
        }
        uint8_t next = (cap >> 8) & 0xffu;
        if (next == 0) break;
        p += next;
    }
}

static bool setup_rings(void)
{
    s_dcbaa = dma_page(&s_dcbaa_phys);
    if (!s_dcbaa) return false;

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

    s_cmd_ring = dma_page(&s_cmd_ring_phys);
    if (!s_cmd_ring) return false;
    ring_init(s_cmd_ring, s_cmd_ring_phys);
    s_cmd_enq = 0; s_cmd_cycle = 1;

    s_evt_ring = (volatile trb_t *)dma_page(&s_evt_ring_phys);
    if (!s_evt_ring) return false;
    s_evt_deq = 0; s_evt_cycle = 1;
    uint32_t *erst = dma_page(&s_erst_phys);
    if (!erst) return false;
    erst[0] = (uint32_t)s_evt_ring_phys;
    erst[1] = (uint32_t)(s_evt_ring_phys >> 32);
    erst[2] = RING_TRBS;

    op_wr32(XOP_CONFIG, cap_rd32(XCAP_HCSPARAMS1) & 0xffu);
    op_wr64(XOP_DCBAAP, s_dcbaa_phys);
    op_wr64(XOP_CRCR, s_cmd_ring_phys | 1u);
    rt_wr32(XRT_IR0 + IR_ERSTSZ, 1u);
    rt_wr64(XRT_IR0 + IR_ERDP, s_evt_ring_phys);
    rt_wr64(XRT_IR0 + IR_ERSTBA, s_erst_phys);
    return true;
}

/* Reset a port; returns speed (0 = not usable). */
static uint32_t reset_port(uint8_t port)
{
    uint32_t sc = portsc_rd(port);
    if (!(sc & PORTSC_CCS)) return 0;
    portsc_wr(port, (sc & ~PORTSC_RW1CS) | PORTSC_PR);
    for (uint32_t i = 0; i < 500000u; ++i) {
        uint32_t v = portsc_rd(port);
        if (v & PORTSC_PRC) {
            portsc_wr(port, (v & ~PORTSC_RW1CS) | PORTSC_PRC | PORTSC_CSC);
            if (v & PORTSC_PED) return (v >> PORTSC_SPEED_SHIFT) & 0xfu;
            return 0;
        }
        spin(1);
    }
    return 0;
}

/* Address a freshly-slotted device (sets up EP0 + input/device contexts). */
static bool address_device(struct xhci_dev *d)
{
    d->dev_ctx = dma_page(&d->dev_ctx_phys);
    d->in_ctx  = dma_page(&d->in_ctx_phys);
    d->ep0.ring = dma_page(&d->ep0.ring_phys);
    if (!d->dev_ctx || !d->in_ctx || !d->ep0.ring) return false;
    s_dcbaa[d->slot_id] = d->dev_ctx_phys;
    ring_init(d->ep0.ring, d->ep0.ring_phys);
    d->ep0.enq = 0; d->ep0.cycle = 1; d->ep0.dci = 1;

    uint32_t *ic = in_ctrl_ctx(d->in_ctx);
    ic[1] = (1u << 0) | (1u << 1);   /* add slot + EP0 */
    uint32_t *sc = slot_ctx(d->in_ctx);
    sc[0] = ((uint32_t)d->speed << 20) | (1u << 27);
    sc[1] = ((uint32_t)d->root_port << 16);

    d->ep0_mps = ep0_mps_for_speed(d->speed);
    uint32_t *e0 = ep_ctx(d->in_ctx, 1);
    e0[1] = (EPTYPE_CONTROL << 3) | (3u << 1) | ((uint32_t)d->ep0_mps << 16);
    e0[2] = (uint32_t)(d->ep0.ring_phys | 1u);
    e0[3] = (uint32_t)(d->ep0.ring_phys >> 32);

    if (cmd_exec(d->in_ctx_phys, TRB_TYPE(TRB_ADDRESS_DEVICE) |
                 ((uint32_t)d->slot_id << 24), NULL) != CC_SUCCESS)
        return false;
    /* Mark usable now so the control transfers in configure_device (which go
       through xhci_control's `used` guard) are accepted. xhci_find_device only
       scans [0, s_ndev), so a device that later fails configure — and is never
       counted in s_ndev — is not visible to callers. */
    d->used = true;
    return true;
}

/* Parse config descriptor, configure endpoints, register the device. */
static bool configure_device(struct xhci_dev *d)
{
    uint64_t buf_phys;
    uint8_t *buf = dma_page(&buf_phys);
    if (!buf) return false;
    int dev_index = (int)(d - s_dev);

    /* device descriptor: refine EP0 max packet size */
    if (xhci_control(dev_index, 0x80, 0x06, 0x0100, 0, 18, buf_phys) != 0) { serial_write("xhci: GET dev-desc failed\n"); return false; }
    if (d->speed <= 3 && buf[7]) d->ep0_mps = buf[7];

    /* config descriptor (9 then full) */
    if (xhci_control(dev_index, 0x80, 0x06, 0x0200, 0, 9, buf_phys) != 0) { serial_write("xhci: GET cfg9 failed\n"); return false; }
    uint16_t total = (uint16_t)(buf[2] | (buf[3] << 8));
    if (total > 4096) total = 4096;
    if (xhci_control(dev_index, 0x80, 0x06, 0x0200, 0, total, buf_phys) != 0) { serial_write("xhci: GET cfgN failed\n"); return false; }
    uint8_t cfg_value = buf[5];

    d->bulk_in = d->bulk_out = d->intr_in = -1;
    d->nep = 0;
    bool have_iface = false;
    uint32_t i = 0;
    while (i + 2 <= total) {
        uint8_t len = buf[i], type = buf[i + 1];
        if (len == 0) break;
        if (type == 0x04 && i + 9 <= total) {         /* INTERFACE */
            if (!have_iface) {   /* record first interface's class triple */
                d->iclass = buf[i + 5]; d->isub = buf[i + 6]; d->iproto = buf[i + 7];
                have_iface = true;
            }
        } else if (type == 0x05 && i + 7 <= total && d->nep < MAX_EP) { /* ENDPOINT */
            uint8_t eaddr = buf[i + 2];
            uint8_t eattr = buf[i + 3] & 0x03u;
            bool in = (eaddr & 0x80u) != 0;

            /* Only bulk (either dir) and interrupt-IN are supported; determine
               the type BEFORE allocating a ring so unsupported endpoints don't
               leak a page. */
            uint8_t eptype;
            if      (eattr == 0x02) eptype = in ? EPTYPE_BULK_IN : EPTYPE_BULK_OUT;
            else if (eattr == 0x03 && in) eptype = EPTYPE_INTR_IN;
            else { i += len; continue; }

            struct ep *e = &d->eps[d->nep];
            e->dci   = (uint8_t)(2u * (eaddr & 0x0fu) + (in ? 1u : 0u));
            e->type  = eptype;
            e->enq   = 0;
            e->cycle = 1;
            e->pending = -1;
            e->ring  = dma_page(&e->ring_phys);
            if (!e->ring) return false;
            ring_init(e->ring, e->ring_phys);

            if (eptype == EPTYPE_BULK_IN)  d->bulk_in  = d->nep;
            if (eptype == EPTYPE_BULK_OUT) d->bulk_out = d->nep;
            if (eptype == EPTYPE_INTR_IN)  d->intr_in  = d->nep;
            d->nep++;
        }
        i += len;
    }
    if (!have_iface) { serial_write("xhci: no interface\n"); return false; }

    /* SET_CONFIGURATION */
    if (xhci_control(dev_index, 0x00, 0x09, cfg_value, 0, 0, 0) != 0) { serial_write("xhci: SET_CONFIG failed\n"); return false; }

    /* Configure Endpoint: A0 + every discovered endpoint. Re-parse descriptors
       for mps/interval so we don't have to carry them in struct ep. */
    memset(d->in_ctx, 0, 4096);
    uint32_t *ic = in_ctrl_ctx(d->in_ctx);
    uint32_t add = 1u;   /* slot */
    uint8_t max_dci = 1;
    for (int k = 0; k < d->nep; ++k) {
        add |= (1u << d->eps[k].dci);
        if (d->eps[k].dci > max_dci) max_dci = d->eps[k].dci;
    }
    ic[1] = add;
    uint32_t *sc = slot_ctx(d->in_ctx);
    sc[0] = ((uint32_t)d->speed << 20) | ((uint32_t)max_dci << 27);
    sc[1] = ((uint32_t)d->root_port << 16);

    /* second descriptor walk to fetch mps/interval per endpoint */
    i = 0;
    while (i + 2 <= total) {
        uint8_t len = buf[i], type = buf[i + 1];
        if (len == 0) break;
        if (type == 0x05 && i + 7 <= total) {
            uint8_t eaddr = buf[i + 2];
            uint8_t eattr = buf[i + 3] & 0x03u;
            uint16_t emps = (uint16_t)(buf[i + 4] | (buf[i + 5] << 8));
            uint8_t  einter = buf[i + 6];
            bool in = (eaddr & 0x80u) != 0;
            uint8_t dci = (uint8_t)(2u * (eaddr & 0x0fu) + (in ? 1u : 0u));
            /* find the matching ep record */
            struct ep *e = NULL;
            for (int k = 0; k < d->nep; ++k) if (d->eps[k].dci == dci) e = &d->eps[k];
            if (e) {
                uint32_t *ec = ep_ctx(d->in_ctx, dci);
                uint32_t interval = 0;
                if (eattr == 0x03) {   /* interrupt: pick a safe interval */
                    interval = (d->speed >= 3)
                        ? (einter ? (uint32_t)(einter - 1) : 3u)   /* HS/SS exponent */
                        : 6u;                                       /* FS/LS ~8ms */
                    if (interval > 15) interval = 15;
                }
                ec[0] = interval << 16;
                ec[1] = ((uint32_t)e->type << 3) | (3u << 1) | ((uint32_t)emps << 16);
                ec[2] = (uint32_t)(e->ring_phys | 1u);
                ec[3] = (uint32_t)(e->ring_phys >> 32);
                /* Max ESIT Payload (high 16) is only valid for periodic
                   endpoints; leaving it non-zero for bulk trips a parameter
                   error in Configure Endpoint. Set it only for interrupt. */
                uint32_t max_esit = (eattr == 0x03) ? (uint32_t)emps : 0u;
                ec[4] = (uint32_t)emps | (max_esit << 16);  /* avg trb len + ESIT */
            }
        }
        i += len;
    }

    if (cmd_exec(d->in_ctx_phys, TRB_TYPE(TRB_CONFIGURE_EP) |
                 ((uint32_t)d->slot_id << 24), NULL) != CC_SUCCESS) {
        serial_write("xhci: CONFIGURE_EP cmd failed\n");
        return false;
    }

    d->used = true;
    serial_write("xhci: device slot ");
    serial_write_dec(d->slot_id);
    serial_write(" class=");
    serial_write_dec(d->iclass);
    serial_write(" (eps bin=");
    serial_write_dec((uint64_t)(d->bulk_in >= 0));
    serial_write(" bout=");
    serial_write_dec((uint64_t)(d->bulk_out >= 0));
    serial_write(" intr=");
    serial_write_dec((uint64_t)(d->intr_in >= 0));
    serial_write(")\n");
    return true;
}

/* ============================ init ====================================== */
int xhci_init(void)
{
    /* Idempotent: the controller is enumerated once; usb_init and usb_hid_init
       both call this but only the first does the work. */
    static bool s_inited;
    if (s_inited) return s_ndev;
    s_inited = true;

    struct pci_addr addr = pci_find_class(0x0Cu, 0x03u);
    if (!addr.valid) return 0;
    uint32_t cc = pci_read32(addr.bus, addr.dev, addr.func, 0x08);
    if (((cc >> 8) & 0xffu) != 0x30u) {
        serial_write("xhci: USB controller is not xHCI, skipping\n");
        return 0;
    }
    serial_write("xhci: found at ");
    serial_write_dec(addr.bus); serial_write(":");
    serial_write_dec(addr.dev); serial_write(".");
    serial_write_dec(addr.func); serial_write("\n");

    pci_enable_busmaster(addr);
    uint64_t bar = pci_read_bar(addr, 0);
    if (bar == 0) { serial_write("xhci: BAR0 not mapped\n"); return 0; }
    for (uint32_t pg = 0; pg < XHCI_MMIO_PAGES; ++pg)
        vmm_map(XHCI_MMIO_VIRT + (uint64_t)pg * 4096u,
                (bar & ~0xfffULL) + (uint64_t)pg * 4096u,
                VMM_WRITABLE | 0x010ULL);
    s_cap = (volatile uint8_t *)(uintptr_t)XHCI_MMIO_VIRT;

    uint8_t  caplen = *(volatile uint8_t *)(s_cap + XCAP_CAPLENGTH);
    uint32_t hcs1 = cap_rd32(XCAP_HCSPARAMS1);
    uint32_t hcc1 = cap_rd32(XCAP_HCCPARAMS1);
    s_op = s_cap + caplen;
    s_rt = s_cap + (cap_rd32(XCAP_RTSOFF) & ~0x1fu);
    s_db = (volatile uint32_t *)(s_cap + (cap_rd32(XCAP_DBOFF) & ~0x3u));
    s_ctx_size = (hcc1 & (1u << 2)) ? 64u : 32u;
    s_max_ports = (hcs1 >> 24) & 0xffu;

    serial_write("xhci: caplen="); serial_write_dec(caplen);
    serial_write(" ports="); serial_write_dec(s_max_ports);
    serial_write(" ctxsz="); serial_write_dec(s_ctx_size);
    serial_write("\n");

    bios_handoff(hcc1);
    if (!controller_reset()) { serial_write("xhci: reset timeout\n"); return 0; }
    if (!setup_rings()) { serial_write("xhci: ring setup failed\n"); return 0; }

    op_wr32(XOP_USBCMD, op_rd32(XOP_USBCMD) | USBCMD_RS);
    spin(50);

    /* Enumerate every connected port. */
    for (uint8_t port = 1; port <= s_max_ports && s_ndev < MAX_DEV; ++port) {
        uint32_t speed = reset_port(port);
        if (speed == 0) continue;

        uint8_t slot = 0;
        if (cmd_exec(0, TRB_TYPE(TRB_ENABLE_SLOT), &slot) != CC_SUCCESS || slot == 0) {
            serial_write("xhci: enable slot failed on port ");
            serial_write_dec(port); serial_write("\n");
            continue;
        }
        struct xhci_dev *d = &s_dev[s_ndev];
        memset(d, 0, sizeof(*d));
        d->slot_id = slot; d->root_port = port; d->speed = speed;
        if (!address_device(d)) {
            serial_write("xhci: address device failed on port ");
            serial_write_dec(port); serial_write("\n");
            continue;
        }
        if (!configure_device(d)) {
            serial_write("xhci: configure failed on port ");
            serial_write_dec(port); serial_write("\n");
            continue;
        }
        s_ndev++;
    }

    serial_write("xhci: ready, ");
    serial_write_dec((uint64_t)s_ndev);
    serial_write(" device(s)\n");
    return s_ndev;
}
