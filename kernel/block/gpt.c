#include "block/gpt.h"

#include <stdint.h>

#include "lib/string.h"
#include "mem/pmm.h"
#include "mem/vmm.h"

/* GPT header magic: "EFI PART" */
#define GPT_SIGNATURE  0x5452415020494645ULL

typedef struct {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t my_lba;
    uint64_t alternate_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  disk_guid[16];
    uint64_t partition_entry_lba;
    uint32_t num_partition_entries;
    uint32_t size_of_partition_entry;
    uint32_t partition_entry_crc32;
} __attribute__((packed)) gpt_header_t;

typedef struct {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attributes;
    uint16_t name[36]; /* UTF-16 */
} __attribute__((packed)) gpt_entry_t;

static const uint8_t zero_guid[16] = {0};

int gpt_read(blkdev_t *dev, gpt_partition_t *parts, int max)
{
    /* Allocate a scratch page for sector reads. */
    uint64_t buf_phys = pmm_alloc_page();
    if (buf_phys == PMM_ALLOC_FAILED) return 0;
    uint8_t *buf = (uint8_t *)(uintptr_t)(vmm_direct_map_base() + buf_phys);

    /* Read LBA 1: GPT header. */
    if (!blkdev_read(dev, 1, 1, buf)) {
        pmm_free_page(buf_phys);
        return 0;
    }

    gpt_header_t hdr;
    memcpy(&hdr, buf, sizeof(hdr));

    if (hdr.signature != GPT_SIGNATURE) {
        pmm_free_page(buf_phys);
        return 0;
    }

    int found = 0;
    uint64_t entry_lba   = hdr.partition_entry_lba;
    uint32_t entry_size  = hdr.size_of_partition_entry;
    uint32_t entry_count = hdr.num_partition_entries;

    /* Validate untrusted on-disk fields before using them as divisors / bounds.
       A zero or oversized entry size would divide-by-zero (#DE) or index past
       the 512-byte sector buffer. GPT entries are 128 bytes and must fit in a
       sector. Also cap the entry count to a sane maximum. */
    if (entry_size < sizeof(gpt_entry_t) || entry_size > 512U ||
        (512U % entry_size) != 0U || entry_count > 4096U) {
        pmm_free_page(buf_phys);
        return 0;
    }

    /* Walk partition entries (one sector holds 512/entry_size entries). */
    uint32_t per_sector = 512U / entry_size;

    for (uint32_t i = 0; i < entry_count && found < max; ++i) {
        uint64_t lba = entry_lba + i / per_sector;
        uint32_t slot = i % per_sector;

        if (slot == 0) {
            if (!blkdev_read(dev, lba, 1, buf)) break;
        }

        gpt_entry_t *e = (gpt_entry_t *)(buf + slot * entry_size);
        /* Skip empty entries (all-zero type GUID). */
        if (memcmp(e->type_guid, zero_guid, 16) == 0) continue;

        parts[found].first_lba = e->first_lba;
        parts[found].last_lba  = e->last_lba;
        ++found;
    }

    pmm_free_page(buf_phys);
    return found;
}
