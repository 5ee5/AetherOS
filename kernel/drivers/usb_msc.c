#include "drivers/usb_msc.h"
#include "drivers/xhci.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/serial.h"
#include "lib/string.h"
#include "mem/pmm.h"
#include "mem/vmm.h"
#include "sync/spinlock.h"

/* ============================================================================
 * USB Bulk-Only Transport (BOT) + SCSI, exposed as a 512-byte block device.
 * Sits on top of the single device enumerated by xhci.c.
 * ========================================================================== */

#define CBW_SIG  0x43425355u   /* "USBC" */
#define CSW_SIG  0x53425355u   /* "USBS" */

typedef struct {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_len;
    uint8_t  flags;       /* 0x80 = data-in, 0x00 = data-out */
    uint8_t  lun;
    uint8_t  cb_len;      /* 1..16 */
    uint8_t  cb[16];
} __attribute__((packed)) cbw_t;

typedef struct {
    uint32_t signature;
    uint32_t tag;
    uint32_t residue;
    uint8_t  status;      /* 0 = pass, 1 = fail, 2 = phase error */
} __attribute__((packed)) csw_t;

/* Scratch DMA page: CBW at offset 0, CSW at offset 64 (both direct-mapped). */
static uint8_t  *s_scratch;
static uint64_t  s_scratch_phys;
static uint32_t  s_tag = 1;

static uint64_t  s_sector_count;
static bool      s_ready;
static int       s_dev = -1;   /* xHCI device handle for the MSC device */
static spinlock_t s_lock = SPINLOCK_INIT;

/* Run one BOT command. `data_phys` (direct-mapped) may be 0 for no data phase.
   Returns true on CSW status == pass. */
static bool bot_command(const uint8_t *cdb, uint8_t cdb_len,
                        uint64_t data_phys, uint32_t data_len, bool data_in)
{
    cbw_t *cbw = (cbw_t *)s_scratch;
    memset(cbw, 0, sizeof(*cbw));
    cbw->signature = CBW_SIG;
    cbw->tag       = s_tag++;
    cbw->data_len  = data_len;
    cbw->flags     = data_in ? 0x80u : 0x00u;
    cbw->lun       = 0;
    cbw->cb_len    = cdb_len;
    memcpy(cbw->cb, cdb, cdb_len);

    /* CBW out (31 bytes). */
    if (xhci_bulk_out(s_dev, s_scratch_phys, 31) < 0) return false;

    /* Data phase. */
    if (data_len && data_phys) {
        int n = data_in ? xhci_bulk_in(s_dev, data_phys, data_len)
                        : xhci_bulk_out(s_dev, data_phys, data_len);
        if (n < 0) return false;
    }

    /* CSW in (13 bytes). */
    csw_t *csw = (csw_t *)(s_scratch + 64);
    memset(csw, 0, sizeof(*csw));
    if (xhci_bulk_in(s_dev, s_scratch_phys + 64, 13) < 0) return false;
    if (csw->signature != CSW_SIG) return false;
    return csw->status == 0;
}

/* ---- SCSI commands ------------------------------------------------------- */
static bool scsi_test_unit_ready(void)
{
    uint8_t cdb[6] = {0};   /* TEST UNIT READY */
    return bot_command(cdb, 6, 0, 0, false);
}

static bool scsi_read_capacity(uint64_t *last_lba, uint32_t *blk_size)
{
    uint8_t cdb[10] = {0};
    cdb[0] = 0x25;   /* READ CAPACITY(10) */
    uint64_t buf_phys = s_scratch_phys + 128;
    uint8_t *buf = s_scratch + 128;
    memset(buf, 0, 8);
    if (!bot_command(cdb, 10, buf_phys, 8, true)) return false;
    /* both fields big-endian */
    *last_lba = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                ((uint32_t)buf[2] << 8) | buf[3];
    *blk_size = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
                ((uint32_t)buf[6] << 8) | buf[7];
    return true;
}

static bool scsi_rw10(bool write, uint64_t lba, uint16_t count, uint64_t buf_phys)
{
    uint8_t cdb[10] = {0};
    cdb[0] = write ? 0x2Au : 0x28u;   /* WRITE(10) / READ(10) */
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);
    cdb[5] = (uint8_t)(lba);
    cdb[7] = (uint8_t)(count >> 8);
    cdb[8] = (uint8_t)(count);
    return bot_command(cdb, 10, buf_phys, (uint32_t)count * 512u, !write);
}

/* ---- Public block API ---------------------------------------------------- */
bool usb_msc_read_sectors(uint64_t lba, uint16_t count, void *buf)
{
    if (!s_ready || count == 0 || count > 8) return false;
    uint64_t phys = (uint64_t)(uintptr_t)buf - vmm_direct_map_base();
    uint64_t f = spinlock_acquire_irqsave(&s_lock);
    bool ok = scsi_rw10(false, lba, count, phys);
    spinlock_release_irqrestore(&s_lock, f);
    return ok;
}

bool usb_msc_write_sectors(uint64_t lba, uint16_t count, const void *buf)
{
    if (!s_ready || count == 0 || count > 8) return false;
    uint64_t phys = (uint64_t)(uintptr_t)buf - vmm_direct_map_base();
    uint64_t f = spinlock_acquire_irqsave(&s_lock);
    bool ok = scsi_rw10(true, lba, count, phys);
    spinlock_release_irqrestore(&s_lock, f);
    return ok;
}

int usb_msc_first(void){ return s_ready ? 0 : -1; }

bool usb_init(void)
{
    /* Bring up the controller + enumerate all devices once; find the MSC one
       (class 0x08, subclass 0x06 SCSI, protocol 0x50 Bulk-Only). */
    xhci_init();
    s_dev = xhci_find_device(0x08, 0x06, 0x50);
    if (s_dev < 0) return false;

    uint64_t sp = pmm_alloc_page();
    if (sp == PMM_ALLOC_FAILED) return false;
    s_scratch = (uint8_t *)(uintptr_t)(vmm_direct_map_base() + sp);
    s_scratch_phys = sp;
    memset(s_scratch, 0, 4096);

    /* Spin up: TEST UNIT READY may fail a few times while the medium settles. */
    bool ready = false;
    for (int i = 0; i < 20 && !ready; ++i) {
        ready = scsi_test_unit_ready();
        if (!ready) for (volatile int j = 0; j < 200000; ++j) {}
    }
    if (!ready) { serial_write("usb-msc: unit not ready\n"); return false; }

    uint64_t last_lba; uint32_t blk;
    if (!scsi_read_capacity(&last_lba, &blk)) {
        serial_write("usb-msc: read capacity failed\n"); return false;
    }
    if (blk != 512) {
        serial_write("usb-msc: unsupported block size\n"); return false;
    }
    s_sector_count = last_lba + 1;
    s_ready = true;

    serial_write("usb-msc: capacity ");
    serial_write_dec(s_sector_count);
    serial_write(" sectors (512B), ready\n");
    return true;
}
