#include "fs/vfs.h"

#include <stddef.h>
#include <stdint.h>

#include "block/blkdev.h"
#include "block/gpt.h"
#include "core/serial.h"
#include "drivers/ahci.h"
#include "fs/ext2.h"
#include "mem/heap.h"

/* ---- AHCI-backed block device -------------------------------------------- */

typedef struct {
    uint8_t port;
} ahci_blkdev_priv_t;

static bool ahci_blkdev_read(blkdev_t *self, uint64_t lba, uint32_t count, void *buf)
{
    ahci_blkdev_priv_t *priv = (ahci_blkdev_priv_t *)self->priv;
    /* ahci_read_sectors handles at most 8 sectors per call. */
    uint8_t *p = (uint8_t *)buf;
    while (count > 0) {
        uint16_t batch = (count > 8) ? 8 : (uint16_t)count;
        if (!ahci_read_sectors(priv->port, lba, batch, p)) return false;
        p     += (uint64_t)batch * 512;
        lba   += batch;
        count -= batch;
    }
    return true;
}

/* ---- Kernel-level open file table ---------------------------------------- */

#define VFS_MAX_OPEN 16

typedef struct {
    bool     in_use;
    uint32_t inode;
    uint64_t offset;
} vfs_file_t;

static vfs_file_t s_files[VFS_MAX_OPEN];

/* ---- State ---------------------------------------------------------------- */

static ext2_fs_t      *s_root_fs;
static blkdev_t        s_blkdev;
static ahci_blkdev_priv_t s_blkpriv;

/* ---- Public API ----------------------------------------------------------- */

bool vfs_init(void)
{
    int disk = ahci_first_disk();
    if (disk < 0) {
        serial_write("vfs: no disk\n");
        return false;
    }

    s_blkpriv.port     = (uint8_t)disk;
    s_blkdev.sector_size  = 512;
    s_blkdev.sector_count = 0; /* not needed for our use */
    s_blkdev.read      = ahci_blkdev_read;
    s_blkdev.priv      = &s_blkpriv;

    /* Find first GPT partition. */
    gpt_partition_t parts[4];
    int np = gpt_read(&s_blkdev, parts, 4);
    if (np == 0) {
        serial_write("vfs: no GPT partitions\n");
        return false;
    }

    s_root_fs = ext2_mount(&s_blkdev, parts[0].first_lba);
    if (!s_root_fs) {
        serial_write("vfs: ext2 mount failed\n");
        return false;
    }

    serial_write("vfs: mounted ext2 on partition 0\n");
    return true;
}

int vfs_open(const char *path)
{
    if (!s_root_fs) return -1;

    uint32_t ino;
    if (!ext2_lookup(s_root_fs, path, &ino)) return -1;

    for (int i = 0; i < VFS_MAX_OPEN; ++i) {
        if (!s_files[i].in_use) {
            s_files[i].in_use = true;
            s_files[i].inode  = ino;
            s_files[i].offset = 0;
            return i;
        }
    }
    return -1; /* no free slot */
}

int64_t vfs_read(int fd, void *buf, uint32_t size)
{
    if (fd < 0 || fd >= VFS_MAX_OPEN || !s_files[fd].in_use) return -1;
    vfs_file_t *f = &s_files[fd];
    int64_t n = ext2_read(s_root_fs, f->inode, f->offset, buf, size);
    if (n > 0) f->offset += (uint64_t)n;
    return n;
}

void vfs_close(int fd)
{
    if (fd < 0 || fd >= VFS_MAX_OPEN) return;
    s_files[fd].in_use = false;
}

uint64_t vfs_file_size(const char *path)
{
    if (!s_root_fs) return UINT64_MAX;
    uint32_t ino;
    if (!ext2_lookup(s_root_fs, path, &ino)) return UINT64_MAX;
    return ext2_file_size(s_root_fs, ino);
}
