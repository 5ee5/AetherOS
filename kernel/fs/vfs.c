#include "fs/vfs.h"

#include <stddef.h>
#include <stdint.h>

#include "block/blkdev.h"
#include "block/gpt.h"
#include "core/serial.h"
#include "drivers/ahci.h"
#include "drivers/nvme.h"
#include "fs/ext2.h"
#include "lib/string.h"
#include "mem/heap.h"
#include "proc/process.h"
#include "sched/sched.h"

/* ---- AHCI-backed block device -------------------------------------------- */

typedef struct {
    uint8_t port;
} ahci_blkdev_priv_t;

static bool ahci_blkdev_read(blkdev_t *self, uint64_t lba, uint32_t count, void *buf)
{
    ahci_blkdev_priv_t *priv = (ahci_blkdev_priv_t *)self->priv;
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

static bool ahci_blkdev_write(blkdev_t *self, uint64_t lba, uint32_t count, const void *buf)
{
    ahci_blkdev_priv_t *priv = (ahci_blkdev_priv_t *)self->priv;
    const uint8_t *p = (const uint8_t *)buf;
    while (count > 0) {
        uint16_t batch = (count > 8) ? 8 : (uint16_t)count;
        if (!ahci_write_sectors(priv->port, lba, batch, p)) return false;
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

/* ---- NVMe-backed block device -------------------------------------------- */

static bool nvme_blkdev_read(blkdev_t *self, uint64_t lba, uint32_t count, void *buf)
{
    (void)self;
    uint8_t *p = (uint8_t *)buf;
    while (count > 0) {
        uint16_t batch = (count > 8) ? 8 : (uint16_t)count;
        if (!nvme_read_sectors(lba, batch, p)) return false;
        p     += (uint64_t)batch * 512;
        lba   += batch;
        count -= batch;
    }
    return true;
}

static bool nvme_blkdev_write(blkdev_t *self, uint64_t lba, uint32_t count, const void *buf)
{
    (void)self;
    const uint8_t *p = (const uint8_t *)buf;
    while (count > 0) {
        uint16_t batch = (count > 8) ? 8 : (uint16_t)count;
        if (!nvme_write_sectors(lba, batch, p)) return false;
        p     += (uint64_t)batch * 512;
        lba   += batch;
        count -= batch;
    }
    return true;
}

/* ---- State ---------------------------------------------------------------- */

static ext2_fs_t      *s_root_fs;
static blkdev_t        s_blkdev;
static ahci_blkdev_priv_t s_blkpriv;

/* ---- Public API ----------------------------------------------------------- */

bool vfs_init(void)
{
    int disk = ahci_first_disk();
    if (disk >= 0) {
        s_blkpriv.port        = (uint8_t)disk;
        s_blkdev.sector_size  = 512;
        s_blkdev.sector_count = 0;
        s_blkdev.read         = ahci_blkdev_read;
        s_blkdev.write        = ahci_blkdev_write;
        s_blkdev.priv         = &s_blkpriv;
    } else if (nvme_first_ns() >= 0) {
        s_blkdev.sector_size  = 512;
        s_blkdev.sector_count = 0;
        s_blkdev.read         = nvme_blkdev_read;
        s_blkdev.write        = nvme_blkdev_write;
        s_blkdev.priv         = NULL;
    } else {
        serial_write("vfs: no disk\n");
        return false;
    }

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

uint32_t vfs_listdir(const char *path, char *buf, uint32_t bufsz, uint32_t flags)
{
    if (!s_root_fs) return 0;
    uint32_t ino;
    if (!ext2_lookup(s_root_fs, path, &ino)) return 0;
    return ext2_list_dir(s_root_fs, ino, buf, bufsz, flags);
}

int64_t vfs_write(int fd, const void *buf, uint32_t size)
{
    if (fd < 0 || fd >= VFS_MAX_OPEN || !s_files[fd].in_use) return -1;
    vfs_file_t *f = &s_files[fd];
    int64_t n = ext2_write(s_root_fs, f->inode, f->offset, buf, size);
    if (n > 0) f->offset += (uint64_t)n;
    return n;
}

int vfs_open_ex(const char *path, int flags)
{
    if (!s_root_fs) return -1;

    uint32_t ino = 0;
    bool found = ext2_lookup(s_root_fs, path, &ino);

    if (!found) {
        if (!(flags & 0x40)) return -1; /* O_CREAT not set */
        struct process *cp = sched_current_process();
        uint32_t cuid = cp ? cp->cred.euid : 0;
        uint32_t cgid = cp ? cp->cred.egid : 0;
        ino = ext2_create(s_root_fs, path, cuid, cgid);
        if (!ino) return -1;
    } else if (flags & 0x200) { /* O_TRUNC */
        ext2_truncate(s_root_fs, ino);
    }

    for (int i = 0; i < VFS_MAX_OPEN; ++i) {
        if (!s_files[i].in_use) {
            s_files[i].in_use = true;
            s_files[i].inode  = ino;
            s_files[i].offset = (flags & 0x400) ? ext2_file_size(s_root_fs, ino) : 0;
            return i;
        }
    }
    return -1;
}

int vfs_creat(const char *path)
{
    if (!s_root_fs) return -1;
    struct process *p = sched_current_process();
    uint32_t uid = p ? p->cred.euid : 0;
    uint32_t gid = p ? p->cred.egid : 0;
    return ext2_create(s_root_fs, path, uid, gid) ? 0 : -1;
}

int vfs_mkdir(const char *path)
{
    if (!s_root_fs) return -1;
    struct process *p = sched_current_process();
    uint32_t uid = p ? p->cred.euid : 0;
    uint32_t gid = p ? p->cred.egid : 0;
    return ext2_mkdir(s_root_fs, path, uid, gid) ? 0 : -1;
}

int vfs_unlink(const char *path)
{
    if (!s_root_fs) return -1;
    return ext2_unlink(s_root_fs, path) ? 0 : -1;
}

/* ---- Working directory ---------------------------------------------------- */

static char s_kernel_cwd[256] = "/";

int vfs_chdir(const char *path)
{
    if (!s_root_fs || !path) return -1;
    /* Verify the path exists and is a directory. */
    uint32_t ino = 0;
    if (!ext2_lookup(s_root_fs, path, &ino)) return -1;

    struct process *proc = sched_current_process();
    char *cwd = proc ? proc->cwd : s_kernel_cwd;

    /* Normalise: handle relative paths (currently the VFS only handles absolute). */
    if (path[0] == '/') {
        strncpy(cwd, path, 255);
        cwd[255] = '\0';
    } else {
        /* Append to current cwd. */
        uint32_t base_len = strlen(cwd);
        if (base_len > 0 && cwd[base_len - 1] != '/') {
            if (base_len < 254) { cwd[base_len] = '/'; cwd[base_len + 1] = '\0'; base_len++; }
        }
        strncat(cwd, path, 255 - base_len);
        cwd[255] = '\0';
    }
    return 0;
}

int vfs_getcwd(char *buf, uint32_t size)
{
    if (!buf || size == 0) return -1;
    struct process *proc = sched_current_process();
    const char *cwd = proc ? proc->cwd : s_kernel_cwd;
    strncpy(buf, cwd, size - 1);
    buf[size - 1] = '\0';
    return 0;
}

int vfs_file_stat(const char *path, uint16_t *out_mode, uint32_t *out_uid, uint32_t *out_gid)
{
    if (!s_root_fs) return -1;
    uint32_t ino;
    if (!ext2_lookup(s_root_fs, path, &ino)) return -1;
    return ext2_inode_stat(s_root_fs, ino, out_mode, out_uid, out_gid) ? 0 : -1;
}

int vfs_chown(const char *path, uint32_t uid, uint32_t gid)
{
    if (!s_root_fs) return -1;
    uint32_t ino;
    if (!ext2_lookup(s_root_fs, path, &ino)) return -1;
    return ext2_inode_chown(s_root_fs, ino, uid, gid) ? 0 : -1;
}

int vfs_chmod(const char *path, uint16_t mode)
{
    if (!s_root_fs) return -1;
    return ext2_chmod(s_root_fs, path, mode);
}

int vfs_stat(const char *path, ext2_stat_t *out)
{
    if (!s_root_fs || !out) return -1;
    uint32_t ino;
    if (!ext2_lookup(s_root_fs, path, &ino)) return -1;
    return ext2_stat_full(s_root_fs, ino, out) ? 0 : -1;
}

int vfs_rename(const char *old_path, const char *new_path)
{
    if (!s_root_fs) return -1;
    return ext2_rename(s_root_fs, old_path, new_path) ? 0 : -1;
}

void vfs_disk_stats(uint64_t *total_bytes, uint64_t *free_bytes)
{
    ext2_disk_stats(s_root_fs, total_bytes, free_bytes);
}
