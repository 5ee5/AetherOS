#include "fs/ext2.h"

#include <stddef.h>
#include <stdint.h>

#include "lib/string.h"
#include "mem/heap.h"
#include "mem/pmm.h"
#include "mem/vmm.h"

/* ---- on-disk structures -------------------------------------------------- */

#define EXT2_SUPER_MAGIC  0xef53U
#define EXT2_ROOT_INO     2U
#define EXT2_FT_REG_FILE  1U
#define EXT2_FT_DIR       2U

typedef struct {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;    /* block_size = 1024 << s_log_block_size */
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    /* EXT2_DYNAMIC_REV fields: */
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    /* (rest ignored) */
    uint8_t  _pad[1024 - 88];
} __attribute__((packed)) ext2_super_t;

typedef struct {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} __attribute__((packed)) ext2_bgd_t;

typedef struct {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;   /* 512-byte blocks */
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15]; /* 12 direct + 1 indirect + 1 dbl + 1 triple */
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl; /* high 32 bits of size for regular files rev>=1 */
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} __attribute__((packed)) ext2_inode_t;

typedef struct {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[];
} __attribute__((packed)) ext2_dirent_t;

/* ---- filesystem handle ---------------------------------------------------- */

struct ext2_fs {
    blkdev_t *dev;
    uint64_t  part_lba;     /* partition start in device sectors */
    uint32_t  block_size;   /* bytes */
    uint32_t  inodes_per_group;
    uint32_t  inode_size;
    uint32_t  first_data_block;
    uint64_t  bgd_block;    /* first block of BGD table */
    uint8_t  *scratch;      /* one block of heap memory */
};

/* ---- I/O helpers ---------------------------------------------------------- */

static uint64_t dmap_base;

static void *pv(uint64_t phys) {
    return (void *)(uintptr_t)(dmap_base + phys);
}

/* Read `block_no` of the filesystem into `out_buf` (must be block_size bytes). */
static bool read_block(ext2_fs_t *fs, uint32_t block_no, void *out_buf)
{
    uint32_t secs_per_block = fs->block_size / 512;
    uint64_t lba = fs->part_lba + (uint64_t)block_no * secs_per_block;

    /* We need a PMM page for AHCI DMA. */
    uint64_t p = pmm_alloc_page();
    if (p == PMM_ALLOC_FAILED) return false;
    void *vp = pv(p);

    bool ok = false;
    for (uint32_t s = 0; s < secs_per_block; s += 8) {
        uint32_t batch = secs_per_block - s;
        if (batch > 8) batch = 8;
        if (!blkdev_read(fs->dev, lba + s, batch, (uint8_t *)vp + s * 512)) goto out;
    }
    memcpy(out_buf, vp, fs->block_size);
    ok = true;
out:
    pmm_free_page(p);
    return ok;
}

/* Read the inode for inode number `ino` into `out`. */
static bool read_inode(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *out)
{
    if (ino == 0) return false;
    uint32_t group  = (ino - 1) / fs->inodes_per_group;
    uint32_t index  = (ino - 1) % fs->inodes_per_group;

    /* Read the block group descriptor for this group. */
    uint32_t bgd_block = (uint32_t)fs->bgd_block;
    uint32_t bgds_per_block = fs->block_size / sizeof(ext2_bgd_t);
    uint32_t bgd_blk_offset = group / bgds_per_block;
    uint32_t bgd_entry_slot = group % bgds_per_block;

    uint8_t *tmp = fs->scratch;
    if (!read_block(fs, bgd_block + bgd_blk_offset, tmp)) return false;
    ext2_bgd_t bgd;
    memcpy(&bgd, tmp + bgd_entry_slot * sizeof(ext2_bgd_t), sizeof(bgd));

    /* Compute which block of the inode table contains this inode. */
    uint32_t inodes_per_block = fs->block_size / fs->inode_size;
    uint32_t inode_block = bgd.bg_inode_table + index / inodes_per_block;
    uint32_t inode_slot  = index % inodes_per_block;

    if (!read_block(fs, inode_block, tmp)) return false;
    memcpy(out, tmp + inode_slot * fs->inode_size, sizeof(ext2_inode_t));
    return true;
}

/* ---- Public API ----------------------------------------------------------- */

ext2_fs_t *ext2_mount(blkdev_t *dev, uint64_t part_offset_lba)
{
    dmap_base = vmm_direct_map_base();

    /* Read superblock: always at byte offset 1024, i.e. LBA 2 in the partition
       (assuming 512-byte sectors and the partition starts at part_offset_lba). */
    uint64_t sb_phys = pmm_alloc_page();
    if (sb_phys == PMM_ALLOC_FAILED) return NULL;
    void *sb_virt = pv(sb_phys);
    bool ok = blkdev_read(dev, part_offset_lba + 2, 2, sb_virt);
    if (!ok) { pmm_free_page(sb_phys); return NULL; }

    ext2_super_t sb;
    memcpy(&sb, sb_virt, sizeof(sb));
    pmm_free_page(sb_phys);

    if (sb.s_magic != EXT2_SUPER_MAGIC) return NULL;

    uint32_t block_size = 1024U << sb.s_log_block_size;

    ext2_fs_t *fs = kmalloc(sizeof(ext2_fs_t));
    if (!fs) return NULL;
    fs->scratch = kmalloc(block_size);
    if (!fs->scratch) { kfree(fs); return NULL; }

    fs->dev              = dev;
    fs->part_lba         = part_offset_lba;
    fs->block_size       = block_size;
    fs->inodes_per_group = sb.s_inodes_per_group;
    fs->inode_size       = (sb.s_rev_level >= 1) ? sb.s_inode_size : 128U;
    fs->first_data_block = sb.s_first_data_block;
    /* BGD table starts at the block after the superblock. */
    fs->bgd_block        = (block_size == 1024) ? 2U : 1U;

    return fs;
}

/* Walk a directory block to find `name` (length `name_len`).
   Returns inode number or 0 if not found. */
static uint32_t dir_find_in_block(const uint8_t *block, uint32_t block_size,
                                  const char *name, uint8_t name_len)
{
    const uint8_t *p   = block;
    const uint8_t *end = block + block_size;
    while (p < end) {
        const ext2_dirent_t *de = (const ext2_dirent_t *)p;
        if (de->rec_len == 0) break;
        if (de->inode != 0 && de->name_len == name_len &&
            memcmp(de->name, name, name_len) == 0) {
            return de->inode;
        }
        p += de->rec_len;
    }
    return 0;
}

bool ext2_lookup(ext2_fs_t *fs, const char *path, uint32_t *inode_out)
{
    if (!path || path[0] != '/') return false;

    uint32_t cur_ino = EXT2_ROOT_INO;
    const char *p = path + 1; /* skip leading '/' */

    while (*p) {
        /* Extract next component. */
        const char *slash = p;
        while (*slash && *slash != '/') ++slash;
        uint8_t name_len = (uint8_t)(slash - p);
        if (name_len == 0) { p = slash + 1; continue; }

        /* Read current directory inode. */
        ext2_inode_t ino;
        if (!read_inode(fs, cur_ino, &ino)) return false;

        /* Search direct blocks. */
        uint32_t found = 0;
        for (int b = 0; b < 12 && !found; ++b) {
            if (ino.i_block[b] == 0) break;
            if (!read_block(fs, ino.i_block[b], fs->scratch)) return false;
            found = dir_find_in_block(fs->scratch, fs->block_size, p, name_len);
        }

        if (!found) return false;
        cur_ino = found;
        p = (*slash) ? slash + 1 : slash;
    }

    *inode_out = cur_ino;
    return true;
}

uint64_t ext2_file_size(ext2_fs_t *fs, uint32_t ino)
{
    ext2_inode_t inode;
    if (!read_inode(fs, ino, &inode)) return 0;
    return inode.i_size;
}

int64_t ext2_read(ext2_fs_t *fs, uint32_t ino, uint64_t off, void *buf, uint32_t size)
{
    ext2_inode_t inode;
    if (!read_inode(fs, ino, &inode)) return -1;

    uint64_t file_size = inode.i_size;
    if (off >= file_size) return 0;
    if (off + size > file_size) size = (uint32_t)(file_size - off);

    uint8_t *dst = (uint8_t *)buf;
    uint32_t remaining = size;
    uint64_t cur_off   = off;

    while (remaining > 0) {
        uint32_t block_idx = (uint32_t)(cur_off / fs->block_size);
        uint32_t block_off = (uint32_t)(cur_off % fs->block_size);

        uint32_t block_no;
        if (block_idx < 12) {
            block_no = inode.i_block[block_idx];
        } else if (block_idx < 12U + fs->block_size / 4U) {
            /* Singly indirect block. */
            uint32_t indir = inode.i_block[12];
            if (indir == 0) break;
            if (!read_block(fs, indir, fs->scratch)) return -1;
            block_no = ((uint32_t *)fs->scratch)[block_idx - 12];
        } else {
            /* Doubly/triply indirect: not supported for M4 (files < ~64 KiB). */
            break;
        }

        if (block_no == 0) break;
        if (!read_block(fs, block_no, fs->scratch)) return -1;

        uint32_t to_copy = fs->block_size - block_off;
        if (to_copy > remaining) to_copy = remaining;
        memcpy(dst, fs->scratch + block_off, to_copy);
        dst       += to_copy;
        cur_off   += to_copy;
        remaining -= to_copy;
    }

    return (int64_t)(size - remaining);
}

uint32_t ext2_lookup_ino(ext2_fs_t *fs, const char *path)
{
    uint32_t ino = 0;
    if (!ext2_lookup(fs, path, &ino)) return 0;
    return ino;
}

static uint32_t list_dir_block(const uint8_t *block, uint32_t block_size,
                                char *buf, uint32_t bufsz, uint32_t written)
{
    const uint8_t *p   = block;
    const uint8_t *end = block + block_size;
    while (p < end) {
        const ext2_dirent_t *de = (const ext2_dirent_t *)p;
        if (de->rec_len == 0) break;
        if (de->inode != 0 && de->name_len > 0) {
            /* Skip . and .. */
            if (!(de->name_len == 1 && de->name[0] == '.') &&
                !(de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.')) {
                uint8_t nlen = de->name_len;
                if (written + nlen + 1 < bufsz) {
                    memcpy(buf + written, de->name, nlen);
                    written += nlen;
                    buf[written++] = '\n';
                }
            }
        }
        p += de->rec_len;
    }
    return written;
}

uint32_t ext2_list_dir(ext2_fs_t *fs, uint32_t dir_ino, char *buf, uint32_t bufsz)
{
    if (!buf || bufsz == 0) return 0;
    ext2_inode_t inode;
    if (!read_inode(fs, dir_ino, &inode)) return 0;

    uint32_t written = 0;
    for (int b = 0; b < 12; ++b) {
        if (inode.i_block[b] == 0) break;
        if (!read_block(fs, inode.i_block[b], fs->scratch)) break;
        written = list_dir_block(fs->scratch, fs->block_size, buf, bufsz, written);
    }
    if (written < bufsz) buf[written] = '\0';
    return written;
}
