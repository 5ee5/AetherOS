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
#include "sync/spinlock.h"

/* One global FS lock. ext2 shares a single `scratch` buffer and s_files[] is a
   global table, so all VFS entry points must serialize — user threads can run
   concurrently on multiple CPUs. The cleanup-attribute guard releases on every
   return path. No VFS function calls another, so the non-recursive lock is safe. */
static spinlock_t vfs_lock = SPINLOCK_INIT;

static inline void vfs_unlock_cleanup(int *held)
{
    (void)held;
    spinlock_release(&vfs_lock);
}

#define VFS_LOCK()                                                      \
    spinlock_acquire(&vfs_lock);                                        \
    int __vfs_guard __attribute__((cleanup(vfs_unlock_cleanup))) = 1;   \
    (void)__vfs_guard

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

/* ---- Path resolution ------------------------------------------------------ */

/* The kernel's own cwd, used when no process context is available. */
static char s_kernel_cwd[256] = "/";

/* Resolve a possibly-relative path to a normalized absolute path in `out`.
   Relative paths are taken against the current process cwd (or the kernel cwd).
   Collapses ".", "..", and redundant slashes. ext2 requires absolute paths. */
static const char *vfs_resolve(const char *path, char *out, uint32_t outsz)
{
    char tmp[512];

    if (path && path[0] == '/') {
        strncpy(tmp, path, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
    } else {
        struct process *proc = sched_current_process();
        const char *cwd = proc ? proc->cwd : s_kernel_cwd;
        strncpy(tmp, cwd, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        uint32_t n = strlen(tmp);
        if ((n == 0 || tmp[n - 1] != '/') && n < sizeof(tmp) - 1) {
            tmp[n] = '/'; tmp[n + 1] = '\0';
        }
        if (path) strncat(tmp, path, sizeof(tmp) - 1 - strlen(tmp));
    }

    /* Normalize components into out. */
    char *o = out;
    *o = '\0';
    const char *p = tmp;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != '/') p++;
        uint32_t len = (uint32_t)(p - start);
        if (len == 1 && start[0] == '.') {
            continue;
        } else if (len == 2 && start[0] == '.' && start[1] == '.') {
            /* Pop the last component from out. */
            char *last = out;
            for (char *r = out; r < o; r++)
                if (*r == '/') last = r;
            o = last;
            *o = '\0';
        } else {
            if ((uint32_t)(o - out) + len + 1U >= outsz) break; /* overflow guard */
            *o++ = '/';
            for (uint32_t i = 0; i < len; i++) *o++ = start[i];
            *o = '\0';
        }
    }
    if (out[0] == '\0') { out[0] = '/'; out[1] = '\0'; }
    return out;
}

int vfs_open(const char *path)
{
    VFS_LOCK();
    if (!s_root_fs) return -1;
    char abuf[512];
    path = vfs_resolve(path, abuf, sizeof(abuf));

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
    VFS_LOCK();
    if (fd < 0 || fd >= VFS_MAX_OPEN || !s_files[fd].in_use) return -1;
    vfs_file_t *f = &s_files[fd];
    int64_t n = ext2_read(s_root_fs, f->inode, f->offset, buf, size);
    if (n > 0) f->offset += (uint64_t)n;
    return n;
}

void vfs_close(int fd)
{
    VFS_LOCK();
    if (fd < 0 || fd >= VFS_MAX_OPEN) return;
    s_files[fd].in_use = false;
}

uint64_t vfs_file_size(const char *path)
{
    VFS_LOCK();
    if (!s_root_fs) return UINT64_MAX;
    char abuf[512];
    path = vfs_resolve(path, abuf, sizeof(abuf));
    uint32_t ino;
    if (!ext2_lookup(s_root_fs, path, &ino)) return UINT64_MAX;
    return ext2_file_size(s_root_fs, ino);
}

uint32_t vfs_listdir(const char *path, char *buf, uint32_t bufsz, uint32_t flags)
{
    VFS_LOCK();
    if (!s_root_fs) return 0;
    char abuf[512];
    path = vfs_resolve(path, abuf, sizeof(abuf));
    uint32_t ino;
    if (!ext2_lookup(s_root_fs, path, &ino)) return 0;
    return ext2_list_dir(s_root_fs, ino, buf, bufsz, flags);
}

int64_t vfs_write(int fd, const void *buf, uint32_t size)
{
    VFS_LOCK();
    if (fd < 0 || fd >= VFS_MAX_OPEN || !s_files[fd].in_use) return -1;
    vfs_file_t *f = &s_files[fd];
    int64_t n = ext2_write(s_root_fs, f->inode, f->offset, buf, size);
    if (n > 0) f->offset += (uint64_t)n;
    return n;
}

int vfs_open_ex(const char *path, int flags)
{
    VFS_LOCK();
    if (!s_root_fs) return -1;
    char abuf[512];
    path = vfs_resolve(path, abuf, sizeof(abuf));

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
    VFS_LOCK();
    if (!s_root_fs) return -1;
    char abuf[512];
    path = vfs_resolve(path, abuf, sizeof(abuf));
    struct process *p = sched_current_process();
    uint32_t uid = p ? p->cred.euid : 0;
    uint32_t gid = p ? p->cred.egid : 0;
    return ext2_create(s_root_fs, path, uid, gid) ? 0 : -1;
}

int vfs_mkdir(const char *path)
{
    VFS_LOCK();
    if (!s_root_fs) return -1;
    char abuf[512];
    path = vfs_resolve(path, abuf, sizeof(abuf));
    struct process *p = sched_current_process();
    uint32_t uid = p ? p->cred.euid : 0;
    uint32_t gid = p ? p->cred.egid : 0;
    return ext2_mkdir(s_root_fs, path, uid, gid) ? 0 : -1;
}

int vfs_unlink(const char *path)
{
    VFS_LOCK();
    if (!s_root_fs) return -1;
    char abuf[512];
    path = vfs_resolve(path, abuf, sizeof(abuf));
    return ext2_unlink(s_root_fs, path) ? 0 : -1;
}

/* ---- Working directory ---------------------------------------------------- */

int vfs_chdir(const char *path)
{
    VFS_LOCK();
    if (!s_root_fs || !path) return -1;

    /* Resolve to a normalized absolute path, then verify it exists. */
    char abuf[512];
    vfs_resolve(path, abuf, sizeof(abuf));
    uint32_t ino = 0;
    if (!ext2_lookup(s_root_fs, abuf, &ino)) return -1;

    struct process *proc = sched_current_process();
    char *cwd = proc ? proc->cwd : s_kernel_cwd;
    strncpy(cwd, abuf, 255);
    cwd[255] = '\0';
    return 0;
}

int vfs_getcwd(char *buf, uint32_t size)
{
    VFS_LOCK();
    if (!buf || size == 0) return -1;
    struct process *proc = sched_current_process();
    const char *cwd = proc ? proc->cwd : s_kernel_cwd;
    strncpy(buf, cwd, size - 1);
    buf[size - 1] = '\0';
    return 0;
}

int vfs_file_stat(const char *path, uint16_t *out_mode, uint32_t *out_uid, uint32_t *out_gid)
{
    VFS_LOCK();
    if (!s_root_fs) return -1;
    char abuf[512];
    path = vfs_resolve(path, abuf, sizeof(abuf));
    uint32_t ino;
    if (!ext2_lookup(s_root_fs, path, &ino)) return -1;
    return ext2_inode_stat(s_root_fs, ino, out_mode, out_uid, out_gid) ? 0 : -1;
}

int vfs_chown(const char *path, uint32_t uid, uint32_t gid)
{
    VFS_LOCK();
    if (!s_root_fs) return -1;
    char abuf[512];
    path = vfs_resolve(path, abuf, sizeof(abuf));
    uint32_t ino;
    if (!ext2_lookup(s_root_fs, path, &ino)) return -1;
    return ext2_inode_chown(s_root_fs, ino, uid, gid) ? 0 : -1;
}

int vfs_chmod(const char *path, uint16_t mode)
{
    VFS_LOCK();
    if (!s_root_fs) return -1;
    char abuf[512];
    path = vfs_resolve(path, abuf, sizeof(abuf));
    return ext2_chmod(s_root_fs, path, mode);
}

int vfs_stat(const char *path, ext2_stat_t *out)
{
    VFS_LOCK();
    if (!s_root_fs || !out) return -1;
    char abuf[512];
    path = vfs_resolve(path, abuf, sizeof(abuf));
    uint32_t ino;
    if (!ext2_lookup(s_root_fs, path, &ino)) return -1;
    return ext2_stat_full(s_root_fs, ino, out) ? 0 : -1;
}

int vfs_rename(const char *old_path, const char *new_path)
{
    VFS_LOCK();
    if (!s_root_fs) return -1;
    char oldbuf[512], newbuf[512];
    vfs_resolve(old_path, oldbuf, sizeof(oldbuf));
    vfs_resolve(new_path, newbuf, sizeof(newbuf));
    return ext2_rename(s_root_fs, oldbuf, newbuf) ? 0 : -1;
}

void vfs_disk_stats(uint64_t *total_bytes, uint64_t *free_bytes)
{
    VFS_LOCK();
    ext2_disk_stats(s_root_fs, total_bytes, free_bytes);
}
