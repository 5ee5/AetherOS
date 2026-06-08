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
    blkdev_t    *dev;
    uint64_t     part_lba;          /* partition start in device sectors */
    uint32_t     block_size;        /* bytes */
    uint32_t     inodes_per_group;
    uint32_t     blocks_per_group;
    uint32_t     inode_size;
    uint32_t     first_data_block;
    uint64_t     bgd_block;         /* first block of BGD table */
    uint8_t     *scratch;           /* one block of heap memory */
    ext2_super_t cached_sb;         /* superblock cached for write-back */
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

/* ---- Write helpers -------------------------------------------------------- */

#define EXT2_IMODE_REG  0x8000U
#define EXT2_IMODE_DIR  0x4000U

/* Write `block_no` from `in_buf` (block_size bytes) to disk. */
static bool write_block(ext2_fs_t *fs, uint32_t block_no, const void *in_buf)
{
    uint32_t secs_per_block = fs->block_size / 512;
    uint64_t lba = fs->part_lba + (uint64_t)block_no * secs_per_block;

    uint64_t phys = pmm_alloc_page();
    if (phys == PMM_ALLOC_FAILED) return false;
    void *vp = pv(phys);
    memcpy(vp, in_buf, fs->block_size);

    bool ok = true;
    for (uint32_t s = 0; s < secs_per_block; s += 8) {
        uint32_t batch = secs_per_block - s;
        if (batch > 8) batch = 8;
        if (!blkdev_write(fs->dev, lba + s, batch, (uint8_t *)vp + s * 512)) {
            ok = false;
            break;
        }
    }
    pmm_free_page(phys);
    return ok;
}

/* Write the cached superblock back to disk. */
static bool write_super(ext2_fs_t *fs)
{
    /* Superblock lives at byte offset 1024 in the partition = part_lba + 2 sectors. */
    uint64_t phys = pmm_alloc_page();
    if (phys == PMM_ALLOC_FAILED) return false;
    void *vp = pv(phys);
    memset(vp, 0, 4096);
    memcpy(vp, &fs->cached_sb, sizeof(ext2_super_t));
    bool ok = blkdev_write(fs->dev, fs->part_lba + 2, 2, vp);
    pmm_free_page(phys);
    return ok;
}

/* Write inode `ino` from `in` back to the inode table. */
static bool write_inode(ext2_fs_t *fs, uint32_t ino, const ext2_inode_t *in)
{
    if (ino == 0) return false;
    uint32_t group = (ino - 1) / fs->inodes_per_group;
    uint32_t index = (ino - 1) % fs->inodes_per_group;

    uint32_t bgds_per_block  = fs->block_size / sizeof(ext2_bgd_t);
    uint32_t bgd_blk_offset  = group / bgds_per_block;
    uint32_t bgd_entry_slot  = group % bgds_per_block;

    uint8_t *tmp = kmalloc(fs->block_size);
    if (!tmp) return false;

    if (!read_block(fs, (uint32_t)fs->bgd_block + bgd_blk_offset, tmp)) {
        kfree(tmp); return false;
    }
    ext2_bgd_t bgd;
    memcpy(&bgd, tmp + bgd_entry_slot * sizeof(ext2_bgd_t), sizeof(bgd));

    uint32_t inodes_per_block = fs->block_size / fs->inode_size;
    uint32_t inode_block      = bgd.bg_inode_table + index / inodes_per_block;
    uint32_t inode_slot       = index % inodes_per_block;

    if (!read_block(fs, inode_block, tmp)) { kfree(tmp); return false; }
    memcpy(tmp + inode_slot * fs->inode_size, in, sizeof(ext2_inode_t));
    bool ok = write_block(fs, inode_block, tmp);
    kfree(tmp);
    return ok;
}

/* Read BGD for group `g` into `bgd_out`. */
static bool read_bgd(ext2_fs_t *fs, uint32_t g, ext2_bgd_t *bgd_out,
                     uint32_t *blk_out, uint32_t *slot_out)
{
    uint32_t bgds_per_block = fs->block_size / sizeof(ext2_bgd_t);
    uint32_t blk  = (uint32_t)fs->bgd_block + g / bgds_per_block;
    uint32_t slot = g % bgds_per_block;
    if (blk_out)  *blk_out  = blk;
    if (slot_out) *slot_out = slot;
    if (!read_block(fs, blk, fs->scratch)) return false;
    memcpy(bgd_out, fs->scratch + slot * sizeof(ext2_bgd_t), sizeof(ext2_bgd_t));
    return true;
}

/* Write BGD for group `g` from `bgd`. */
static bool write_bgd(ext2_fs_t *fs, uint32_t g, const ext2_bgd_t *bgd)
{
    uint32_t bgds_per_block = fs->block_size / sizeof(ext2_bgd_t);
    uint32_t blk  = (uint32_t)fs->bgd_block + g / bgds_per_block;
    uint32_t slot = g % bgds_per_block;

    uint8_t *tmp = kmalloc(fs->block_size);
    if (!tmp) return false;
    if (!read_block(fs, blk, tmp)) { kfree(tmp); return false; }
    memcpy(tmp + slot * sizeof(ext2_bgd_t), bgd, sizeof(ext2_bgd_t));
    bool ok = write_block(fs, blk, tmp);
    kfree(tmp);
    return ok;
}

/* Allocate a data block.  Returns block number or 0 on failure. */
static uint32_t alloc_block(ext2_fs_t *fs)
{
    uint32_t num_groups = (fs->cached_sb.s_blocks_count +
                           fs->blocks_per_group - 1) / fs->blocks_per_group;

    for (uint32_t g = 0; g < num_groups; g++) {
        ext2_bgd_t bgd;
        if (!read_bgd(fs, g, &bgd, NULL, NULL)) continue;
        if (bgd.bg_free_blocks_count == 0) continue;

        uint8_t *bmap = kmalloc(fs->block_size);
        if (!bmap) return 0;
        if (!read_block(fs, bgd.bg_block_bitmap, bmap)) { kfree(bmap); continue; }

        uint32_t blocks_in_group = fs->blocks_per_group;
        if ((g + 1) * fs->blocks_per_group > fs->cached_sb.s_blocks_count)
            blocks_in_group = fs->cached_sb.s_blocks_count - g * fs->blocks_per_group;

        uint32_t bit = UINT32_MAX;
        for (uint32_t i = 0; i < blocks_in_group; i++) {
            if (!(bmap[i / 8] & (1U << (i % 8)))) { bit = i; break; }
        }
        if (bit == UINT32_MAX) { kfree(bmap); continue; }

        bmap[bit / 8] |= (1U << (bit % 8));
        write_block(fs, bgd.bg_block_bitmap, bmap);
        kfree(bmap);

        bgd.bg_free_blocks_count--;
        write_bgd(fs, g, &bgd);

        fs->cached_sb.s_free_blocks_count--;
        write_super(fs);

        uint32_t block_no = g * fs->blocks_per_group + fs->first_data_block + bit;
        /* Zero the new block. */
        uint8_t *zbuf = kmalloc(fs->block_size);
        if (zbuf) { memset(zbuf, 0, fs->block_size); write_block(fs, block_no, zbuf); kfree(zbuf); }
        return block_no;
    }
    return 0;
}

/* Free a data block. */
static void free_block(ext2_fs_t *fs, uint32_t block_no)
{
    if (block_no < fs->first_data_block) return;
    uint32_t idx = block_no - fs->first_data_block;
    uint32_t g   = idx / fs->blocks_per_group;
    uint32_t bit = idx % fs->blocks_per_group;

    ext2_bgd_t bgd;
    if (!read_bgd(fs, g, &bgd, NULL, NULL)) return;

    uint8_t *bmap = kmalloc(fs->block_size);
    if (!bmap) return;
    if (!read_block(fs, bgd.bg_block_bitmap, bmap)) { kfree(bmap); return; }

    bmap[bit / 8] &= ~(1U << (bit % 8));
    write_block(fs, bgd.bg_block_bitmap, bmap);
    kfree(bmap);

    bgd.bg_free_blocks_count++;
    write_bgd(fs, g, &bgd);

    fs->cached_sb.s_free_blocks_count++;
    write_super(fs);
}

/* Allocate an inode.  Returns inode number (1-based) or 0 on failure. */
static uint32_t alloc_inode(ext2_fs_t *fs)
{
    uint32_t num_groups = (fs->cached_sb.s_inodes_count +
                           fs->inodes_per_group - 1) / fs->inodes_per_group;

    for (uint32_t g = 0; g < num_groups; g++) {
        ext2_bgd_t bgd;
        if (!read_bgd(fs, g, &bgd, NULL, NULL)) continue;
        if (bgd.bg_free_inodes_count == 0) continue;

        uint8_t *bmap = kmalloc(fs->block_size);
        if (!bmap) return 0;
        if (!read_block(fs, bgd.bg_inode_bitmap, bmap)) { kfree(bmap); continue; }

        uint32_t bit = UINT32_MAX;
        for (uint32_t i = 0; i < fs->inodes_per_group; i++) {
            if (!(bmap[i / 8] & (1U << (i % 8)))) { bit = i; break; }
        }
        if (bit == UINT32_MAX) { kfree(bmap); continue; }

        bmap[bit / 8] |= (1U << (bit % 8));
        write_block(fs, bgd.bg_inode_bitmap, bmap);
        kfree(bmap);

        bgd.bg_free_inodes_count--;
        write_bgd(fs, g, &bgd);

        fs->cached_sb.s_free_inodes_count--;
        write_super(fs);

        return g * fs->inodes_per_group + bit + 1;
    }
    return 0;
}

/* Free inode `ino`. */
static void free_inode(ext2_fs_t *fs, uint32_t ino)
{
    if (ino < 2) return;
    uint32_t g   = (ino - 1) / fs->inodes_per_group;
    uint32_t bit = (ino - 1) % fs->inodes_per_group;

    ext2_bgd_t bgd;
    if (!read_bgd(fs, g, &bgd, NULL, NULL)) return;

    uint8_t *bmap = kmalloc(fs->block_size);
    if (!bmap) return;
    if (!read_block(fs, bgd.bg_inode_bitmap, bmap)) { kfree(bmap); return; }

    bmap[bit / 8] &= ~(1U << (bit % 8));
    write_block(fs, bgd.bg_inode_bitmap, bmap);
    kfree(bmap);

    bgd.bg_free_inodes_count++;
    write_bgd(fs, g, &bgd);

    fs->cached_sb.s_free_inodes_count++;
    write_super(fs);
}

/* Split an absolute path into parent dir path and leaf name.
   e.g. "/foo/bar" -> parent="/foo", name="bar"
        "/foo"     -> parent="/",    name="foo"  */
static bool path_split(const char *path, char *parent, char *name)
{
    const char *last = path;
    for (const char *p = path; *p; p++)
        if (*p == '/') last = p;

    uint32_t name_len = 0;
    const char *n = last + 1;
    while (n[name_len]) name_len++;
    if (name_len == 0 || name_len > 254) return false;
    memcpy(name, n, name_len);
    name[name_len] = '\0';

    uint32_t par_len = (uint32_t)(last - path);
    if (par_len == 0) { parent[0] = '/'; parent[1] = '\0'; }
    else { memcpy(parent, path, par_len); parent[par_len] = '\0'; }
    return true;
}

/* Add a directory entry to dir_ino.  Returns true on success. */
static bool dir_add_entry(ext2_fs_t *fs, uint32_t dir_ino,
                          const char *name, uint32_t child_ino, uint8_t ftype)
{
    uint8_t  name_len = (uint8_t)strlen(name);
    uint32_t needed   = (uint32_t)(8 + ((name_len + 3) & ~3U));

    ext2_inode_t dir_inode;
    if (!read_inode(fs, dir_ino, &dir_inode)) return false;

    uint8_t *blk = kmalloc(fs->block_size);
    if (!blk) return false;

    /* Search existing blocks for slack space. */
    for (int b = 0; b < 12; b++) {
        uint32_t bno = dir_inode.i_block[b];
        if (bno == 0) break;
        if (!read_block(fs, bno, blk)) continue;

        uint8_t *p = blk;
        uint8_t *end = blk + fs->block_size;
        while (p < end) {
            ext2_dirent_t *de = (ext2_dirent_t *)p;
            if (de->rec_len == 0) break;
            uint32_t actual = (uint32_t)(8 + ((de->name_len + 3) & ~3U));
            if (de->inode == 0 && de->rec_len >= needed) {
                /* Reuse a deleted slot. */
                de->inode     = child_ino;
                de->name_len  = name_len;
                de->file_type = ftype;
                memcpy(de->name, name, name_len);
                write_block(fs, bno, blk);
                kfree(blk);
                return true;
            }
            /* Check if last entry has enough slack. */
            uint8_t *next = p + de->rec_len;
            if (next >= end && de->inode != 0) {
                uint32_t slack = de->rec_len - actual;
                if (slack >= needed) {
                    /* Shrink current, append new entry. */
                    de->rec_len = (uint16_t)actual;
                    ext2_dirent_t *ne = (ext2_dirent_t *)(p + actual);
                    ne->inode     = child_ino;
                    ne->rec_len   = (uint16_t)slack;
                    ne->name_len  = name_len;
                    ne->file_type = ftype;
                    memcpy(ne->name, name, name_len);
                    write_block(fs, bno, blk);
                    kfree(blk);
                    return true;
                }
            }
            p += de->rec_len;
        }
    }

    /* Need a new block for the directory. */
    uint32_t new_bno = alloc_block(fs);
    if (!new_bno) { kfree(blk); return false; }

    memset(blk, 0, fs->block_size);
    ext2_dirent_t *ne = (ext2_dirent_t *)blk;
    ne->inode     = child_ino;
    ne->rec_len   = (uint16_t)fs->block_size;
    ne->name_len  = name_len;
    ne->file_type = ftype;
    memcpy(ne->name, name, name_len);
    write_block(fs, new_bno, blk);
    kfree(blk);

    /* Attach new block to the directory inode. */
    for (int b = 0; b < 12; b++) {
        if (dir_inode.i_block[b] == 0) {
            dir_inode.i_block[b] = new_bno;
            dir_inode.i_size    += fs->block_size;
            dir_inode.i_blocks  += fs->block_size / 512;
            write_inode(fs, dir_ino, &dir_inode);
            return true;
        }
    }
    free_block(fs, new_bno);
    return false;
}

/* Remove a directory entry by name.  Sets inode=0 (soft delete). */
static bool dir_remove_entry(ext2_fs_t *fs, uint32_t dir_ino, const char *name)
{
    uint8_t   name_len = (uint8_t)strlen(name);
    ext2_inode_t dir_inode;
    if (!read_inode(fs, dir_ino, &dir_inode)) return false;

    uint8_t *blk = kmalloc(fs->block_size);
    if (!blk) return false;

    for (int b = 0; b < 12; b++) {
        uint32_t bno = dir_inode.i_block[b];
        if (bno == 0) break;
        if (!read_block(fs, bno, blk)) continue;

        uint8_t *p = blk, *end = blk + fs->block_size;
        while (p < end) {
            ext2_dirent_t *de = (ext2_dirent_t *)p;
            if (de->rec_len == 0) break;
            if (de->inode != 0 && de->name_len == name_len &&
                memcmp(de->name, name, name_len) == 0) {
                de->inode = 0;
                write_block(fs, bno, blk);
                kfree(blk);
                return true;
            }
            p += de->rec_len;
        }
    }
    kfree(blk);
    return false;
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
    fs->blocks_per_group = sb.s_blocks_per_group;
    fs->inode_size       = (sb.s_rev_level >= 1) ? sb.s_inode_size : 128U;
    fs->first_data_block = sb.s_first_data_block;
    fs->bgd_block        = (block_size == 1024) ? 2U : 1U;
    memcpy(&fs->cached_sb, &sb, sizeof(ext2_super_t));

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

/* ---- Write API ------------------------------------------------------------- */

int64_t ext2_write(ext2_fs_t *fs, uint32_t ino, uint64_t off,
                   const void *buf, uint32_t size)
{
    if (!fs || ino == 0 || size == 0) return -1;
    ext2_inode_t inode;
    if (!read_inode(fs, ino, &inode)) return -1;

    const uint8_t *src = (const uint8_t *)buf;
    uint32_t written = 0;

    uint8_t *blkbuf = kmalloc(fs->block_size);
    if (!blkbuf) return -1;

    while (written < size) {
        uint32_t block_idx  = (uint32_t)((off + written) / fs->block_size);
        uint32_t block_off  = (uint32_t)((off + written) % fs->block_size);
        uint32_t to_write   = fs->block_size - block_off;
        if (to_write > size - written) to_write = size - written;

        if (block_idx >= 12) break; /* only direct blocks */

        uint32_t bno = inode.i_block[block_idx];
        if (bno == 0) {
            bno = alloc_block(fs);
            if (!bno) break;
            inode.i_block[block_idx] = bno;
        }

        if (block_off > 0 || to_write < fs->block_size) {
            if (!read_block(fs, bno, blkbuf)) break;
        } else {
            memset(blkbuf, 0, fs->block_size);
        }
        memcpy(blkbuf + block_off, src + written, to_write);
        if (!write_block(fs, bno, blkbuf)) break;

        written += to_write;
    }
    kfree(blkbuf);

    if (written > 0) {
        uint64_t new_end = off + written;
        if (new_end > inode.i_size) {
            inode.i_size = (uint32_t)new_end;
            /* Update i_blocks (512-byte units). */
            uint32_t blks = 0;
            for (int b = 0; b < 12; b++)
                if (inode.i_block[b]) blks++;
            inode.i_blocks = blks * (fs->block_size / 512);
        }
        write_inode(fs, ino, &inode);
    }
    return (int64_t)written;
}

bool ext2_truncate(ext2_fs_t *fs, uint32_t ino)
{
    if (!fs || ino < 2) return false;
    ext2_inode_t inode;
    if (!read_inode(fs, ino, &inode)) return false;

    for (int b = 0; b < 12; b++) {
        if (inode.i_block[b]) {
            free_block(fs, inode.i_block[b]);
            inode.i_block[b] = 0;
        }
    }
    inode.i_size   = 0;
    inode.i_blocks = 0;
    return write_inode(fs, ino, &inode);
}

uint32_t ext2_create(ext2_fs_t *fs, const char *path)
{
    if (!fs || !path) return 0;
    char parent[256], name[256];
    if (!path_split(path, parent, name)) return 0;

    uint32_t parent_ino = ext2_lookup_ino(fs, parent);
    if (!parent_ino) return 0;

    /* Check if already exists. */
    if (ext2_lookup_ino(fs, path)) return 0;

    uint32_t ino = alloc_inode(fs);
    if (!ino) return 0;

    ext2_inode_t inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode   = EXT2_IMODE_REG | 0644U;
    inode.i_links_count = 1;
    inode.i_size   = 0;
    if (!write_inode(fs, ino, &inode)) { free_inode(fs, ino); return 0; }

    if (!dir_add_entry(fs, parent_ino, name, ino, 1 /* EXT2_FT_REG_FILE */)) {
        free_inode(fs, ino);
        return 0;
    }
    return ino;
}

uint32_t ext2_mkdir(ext2_fs_t *fs, const char *path)
{
    if (!fs || !path) return 0;
    char parent[256], name[256];
    if (!path_split(path, parent, name)) return 0;

    uint32_t parent_ino = ext2_lookup_ino(fs, parent);
    if (!parent_ino) return 0;

    if (ext2_lookup_ino(fs, path)) return 0; /* already exists */

    uint32_t ino = alloc_inode(fs);
    if (!ino) return 0;

    ext2_inode_t inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode   = EXT2_IMODE_DIR | 0755U;
    inode.i_links_count = 2; /* . and parent */
    if (!write_inode(fs, ino, &inode)) { free_inode(fs, ino); return 0; }

    /* Add . and .. into the new directory. */
    if (!dir_add_entry(fs, ino, ".", ino, 2 /* EXT2_FT_DIR */) ||
        !dir_add_entry(fs, ino, "..", parent_ino, 2)) {
        free_inode(fs, ino);
        return 0;
    }

    /* Add entry in parent. */
    if (!dir_add_entry(fs, parent_ino, name, ino, 2)) {
        free_inode(fs, ino);
        return 0;
    }

    /* Increment parent link count for the new subdir. */
    ext2_inode_t par;
    if (read_inode(fs, parent_ino, &par)) {
        par.i_links_count++;
        write_inode(fs, parent_ino, &par);
    }

    return ino;
}

bool ext2_unlink(ext2_fs_t *fs, const char *path)
{
    if (!fs || !path) return false;
    char parent[256], name[256];
    if (!path_split(path, parent, name)) return false;

    uint32_t parent_ino = ext2_lookup_ino(fs, parent);
    uint32_t ino        = ext2_lookup_ino(fs, path);
    if (!parent_ino || !ino) return false;

    ext2_inode_t inode;
    if (!read_inode(fs, ino, &inode)) return false;

    if (inode.i_links_count > 0) inode.i_links_count--;

    if (inode.i_links_count == 0) {
        /* Free data blocks. */
        for (int b = 0; b < 12; b++) {
            if (inode.i_block[b]) { free_block(fs, inode.i_block[b]); inode.i_block[b] = 0; }
        }
        inode.i_size   = 0;
        inode.i_blocks = 0;
        write_inode(fs, ino, &inode);
        free_inode(fs, ino);
    } else {
        write_inode(fs, ino, &inode);
    }

    return dir_remove_entry(fs, parent_ino, name);
}
