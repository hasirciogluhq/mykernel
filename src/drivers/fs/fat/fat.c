/*
 * FAT12/FAT16/FAT32 read/write filesystem driver.
 *
 * Read side:
 *   - Parses BPB (12/16/32 auto-detection by cluster count).
 *   - Traverses directory entries in the fixed-size root area
 *     (FAT12/16) or the clustered root (FAT32).
 *   - Reads regular files by walking the cluster chain via the FAT.
 *
 * Write side:
 *   - Allocates new clusters by linear scan of the FAT for the first
 *     free (0x00000000 / 0x0000 / 0x000) entry.
 *   - Extends a file's cluster chain when writing past its current
 *     tail, updating both the previous EOC and the directory entry's
 *     first-cluster / size fields.
 *   - Writes go through the block API sector by sector.
 *
 * readdir returns the 8.3 short name for each entry (long file name
 * entries are recognized and skipped — we always report the short
 * name in that case).
 *
 * Not implemented: mkdir/rmdir/rename/unlink (only files can be
 * grown / rewritten in place).
 */

#include <kernel/vfs_api.h>
#include <kernel/block_api.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/string.h>
#include <drivers/vfs_fs.h>
#include <drivers/driver.h>
#include <drivers/vga.h>

static dentry_t *(*alloc_dentry)(const char *, dentry_t *, inode_t *);
static inode_t  *(*alloc_inode)(super_block_t *, uint32_t);

typedef struct {
    block_device_t *bdev;
    uint16_t bytes_per_sec;
    uint8_t  sec_per_clust;
    uint16_t reserved;
    uint8_t  fats;
    uint32_t fat_sectors;
    uint32_t root_clust;
    uint32_t data_start;
    uint32_t root_ents;
    int      fat_bits;
    uint32_t clusters;
} fat_fs_t;

typedef struct {
    fat_fs_t *fs;
    uint32_t  first_clust;
    uint32_t  size;
    int       is_dir;

    /* location of this file's 32-byte directory entry, so we can
     * update size / first_cluster on write.  For the root directory
     * these are zero and updates are skipped. */
    uint32_t  dir_ent_clust;  /* cluster containing the entry (0 = FAT12/16 root) */
    uint32_t  dir_ent_sector; /* absolute LBA of the sector holding the entry */
    uint32_t  dir_ent_offset; /* byte offset within that sector */
} fat_node_t;

static const inode_operations_t fat_iops;
static const file_operations_t  fat_fops;

/* ---------------- Sector-level I/O ---------------- */

static int fat_read_sector(fat_fs_t *fs, uint32_t lba, void *buf)
{
    const block_api_t *api = block_api_get();
    if (!api || !api->read)
        return -EIO;
    return api->read(fs->bdev, lba, buf, 1);
}

static int fat_write_sector(fat_fs_t *fs, uint32_t lba, const void *buf)
{
    const block_api_t *api = block_api_get();
    if (!api || !api->write)
        return -EIO;
    return api->write(fs->bdev, lba, buf, 1);
}

static uint32_t clust_to_lba(fat_fs_t *fs, uint32_t cl)
{
    return fs->data_start + (cl - 2) * fs->sec_per_clust;
}

/* ---------------- FAT walk ---------------- */

static uint32_t fat_eoc_value(fat_fs_t *fs)
{
    if (fs->fat_bits == 12) return 0x0FF8;
    if (fs->fat_bits == 16) return 0xFFF8;
    return 0x0FFFFFF8u;
}

static uint32_t fat_next(fat_fs_t *fs, uint32_t cl)
{
    uint8_t *sec = (uint8_t *)kmalloc(512);
    uint32_t fat_off, lba, ent = 0;
    if (!sec)
        return 0xFFFFFFFFu;
    if (fs->fat_bits == 32) {
        fat_off = cl * 4;
        lba = fs->reserved + (fat_off / fs->bytes_per_sec);
        if (fat_read_sector(fs, lba, sec) < 0)
            return 0xFFFFFFFFu;
        ent = *(uint32_t *)(sec + (fat_off % fs->bytes_per_sec)) & 0x0FFFFFFFu;
        return ent;
    }
    if (fs->fat_bits == 16) {
        fat_off = cl * 2;
        lba = fs->reserved + (fat_off / fs->bytes_per_sec);
        if (fat_read_sector(fs, lba, sec) < 0)
            return 0xFFFFFFFFu;
        return *(uint16_t *)(sec + (fat_off % fs->bytes_per_sec));
    }
    /* FAT12 */
    fat_off = cl + (cl / 2);
    lba = fs->reserved + (fat_off / fs->bytes_per_sec);
    if (fat_read_sector(fs, lba, sec) < 0)
        return 0xFFFFFFFFu;
    {
        uint16_t w = *(uint16_t *)(sec + (fat_off % fs->bytes_per_sec));
        ent = (cl & 1) ? (uint32_t)(w >> 4) : (uint32_t)(w & 0x0FFF);
    }
    return ent;
}

static int fat_write_entry(fat_fs_t *fs, uint32_t cl, uint32_t val)
{
    uint32_t fat_off;
    uint32_t lba;
    uint8_t *sec;
    uint32_t f;
    int rc = 0;

    sec = (uint8_t *)kmalloc(512);
    if (!sec) return -ENOMEM;

    /* Write to every FAT copy. */
    for (f = 0; f < fs->fats; f++) {
        uint32_t fat_base = fs->reserved + f * fs->fat_sectors;
        if (fs->fat_bits == 32) {
            fat_off = cl * 4;
            lba = fat_base + (fat_off / fs->bytes_per_sec);
            if (fat_read_sector(fs, lba, sec) < 0) { rc = -EIO; continue; }
            {
                uint32_t *slot = (uint32_t *)(sec + (fat_off % fs->bytes_per_sec));
                *slot = (*slot & 0xF0000000u) | (val & 0x0FFFFFFFu);
            }
            if (fat_write_sector(fs, lba, sec) < 0) rc = -EIO;
        } else if (fs->fat_bits == 16) {
            fat_off = cl * 2;
            lba = fat_base + (fat_off / fs->bytes_per_sec);
            if (fat_read_sector(fs, lba, sec) < 0) { rc = -EIO; continue; }
            *(uint16_t *)(sec + (fat_off % fs->bytes_per_sec)) = (uint16_t)val;
            if (fat_write_sector(fs, lba, sec) < 0) rc = -EIO;
        } else {
            /* FAT12 */
            fat_off = cl + (cl / 2);
            lba = fat_base + (fat_off / fs->bytes_per_sec);
            if (fat_read_sector(fs, lba, sec) < 0) { rc = -EIO; continue; }
            {
                uint16_t *w = (uint16_t *)(sec + (fat_off % fs->bytes_per_sec));
                if (cl & 1)
                    *w = (uint16_t)((*w & 0x000F) | ((val & 0x0FFF) << 4));
                else
                    *w = (uint16_t)((*w & 0xF000) | (val & 0x0FFF));
            }
            if (fat_write_sector(fs, lba, sec) < 0) rc = -EIO;
        }
    }
    return rc;
}

static uint32_t fat_alloc_cluster(fat_fs_t *fs)
{
    uint32_t cl;
    for (cl = 2; cl < fs->clusters + 2; cl++) {
        uint32_t v = fat_next(fs, cl);
        if (fs->fat_bits == 32) v &= 0x0FFFFFFFu;
        if (v == 0) {
            if (fat_write_entry(fs, cl, fat_eoc_value(fs) | (fs->fat_bits == 32 ? 0x0FFFFFFFu :
                                                            (fs->fat_bits == 16 ? 0xFFFFu : 0x0FFFu))) < 0)
                return 0;
            /* Zero the new cluster on disk. */
            {
                uint8_t *zero = (uint8_t *)kmalloc(512);
                uint32_t i;
                if (!zero) return 0;
                memset(zero, 0, 512);
                for (i = 0; i < fs->sec_per_clust; i++)
                    (void)fat_write_sector(fs, clust_to_lba(fs, cl) + i, zero);
            }
            return cl;
        }
    }
    return 0;
}

/* ---------------- Directory entry name conversion ---------------- */

static void fat83_to_name(const uint8_t *e, char *out)
{
    int i, j = 0;
    for (i = 0; i < 8 && e[i] != ' '; i++)
        out[j++] = (char)e[i];
    if (e[8] != ' ') {
        out[j++] = '.';
        for (i = 8; i < 11 && e[i] != ' '; i++)
            out[j++] = (char)e[i];
    }
    out[j] = 0;
    for (i = 0; out[i]; i++) {
        if (out[i] >= 'A' && out[i] <= 'Z')
            out[i] = (char)(out[i] - 'A' + 'a');
    }
}

static int name_eq(const char *a, const char *b)
{
    char ca, cb;
    while (*a && *b) {
        ca = *a; cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* ---------------- Directory iteration ---------------- */

/*
 * Iterate directory entries.  For each valid (non-deleted, non-LFN,
 * non-volume) entry, call cb().  The callback receives the raw 32-byte
 * directory entry buffer plus the sector LBA and offset it lives at,
 * so it can update size and first-cluster fields when writing.
 *
 * Return 1 from cb to stop iteration.
 */
typedef int (*fat_ent_cb)(void *ctx, const uint8_t *ent, uint32_t sec_lba,
                          uint32_t off_in_sec, uint32_t containing_cluster);

static int fat_scan_dir_root_area(fat_fs_t *fs, fat_ent_cb cb, void *ctx)
{
    uint32_t root_secs = (fs->root_ents * 32 + fs->bytes_per_sec - 1) / fs->bytes_per_sec;
    uint32_t root_lba  = fs->reserved + fs->fats * fs->fat_sectors;
    uint8_t *sec = (uint8_t *)kmalloc(512);
    uint32_t i;
    uint32_t e;

    if (!sec) return -ENOMEM;
    for (i = 0; i < root_secs; i++) {
        if (fat_read_sector(fs, root_lba + i, sec) < 0)
            return -EIO;
        for (e = 0; e < 512; e += 32) {
            if (sec[e] == 0)
                return 0;
            if (sec[e] == 0xE5)
                continue;
            if ((sec[e + 11] & 0x0F) == 0x0F)
                continue; /* LFN */
            if (sec[e + 11] & 0x08)
                continue; /* volume label */
            {
                int rc = cb(ctx, sec + e, root_lba + i, e, 0);
                if (rc)
                    return rc;
            }
        }
    }
    return 0;
}

static int fat_scan_dir_cluster_chain(fat_fs_t *fs, uint32_t start_clust,
                                      fat_ent_cb cb, void *ctx)
{
    uint8_t *sec = (uint8_t *)kmalloc(512);
    uint32_t cl = start_clust;

    if (!sec) return -ENOMEM;
    while (cl >= 2 && cl < 0x0FFFFFF8u) {
        uint32_t s;
        for (s = 0; s < fs->sec_per_clust; s++) {
            uint32_t lba = clust_to_lba(fs, cl) + s;
            uint32_t e;
            if (fat_read_sector(fs, lba, sec) < 0)
                return -EIO;
            for (e = 0; e < 512; e += 32) {
                if (sec[e] == 0)
                    return 0;
                if (sec[e] == 0xE5)
                    continue;
                if ((sec[e + 11] & 0x0F) == 0x0F)
                    continue;
                if (sec[e + 11] & 0x08)
                    continue;
                {
                    int rc = cb(ctx, sec + e, lba, e, cl);
                    if (rc)
                        return rc;
                }
            }
        }
        cl = fat_next(fs, cl);
    }
    return 0;
}

static int fat_scan_dir(fat_fs_t *fs, uint32_t dir_clust, fat_ent_cb cb, void *ctx)
{
    if (fs->fat_bits != 32 && dir_clust == 0)
        return fat_scan_dir_root_area(fs, cb, ctx);
    if (dir_clust == 0)
        dir_clust = fs->root_clust;
    return fat_scan_dir_cluster_chain(fs, dir_clust, cb, ctx);
}

/* ---------------- Lookup ---------------- */

typedef struct {
    const char *want;
    int         found;
    int         is_dir;
    uint32_t    first_clust;
    uint32_t    size;
    uint32_t    ent_lba;
    uint32_t    ent_off;
} fat_lookup_ctx_t;

static int fat_lookup_cb(void *ctx, const uint8_t *ent, uint32_t sec_lba,
                         uint32_t off_in_sec, uint32_t cclust)
{
    fat_lookup_ctx_t *c = (fat_lookup_ctx_t *)ctx;
    char nm[16];
    (void)cclust;
    fat83_to_name(ent, nm);
    if (name_eq(nm, c->want)) {
        uint16_t lo = *(uint16_t *)(ent + 26);
        uint16_t hi = *(uint16_t *)(ent + 20);
        c->first_clust = ((uint32_t)hi << 16) | lo;
        c->size        = *(uint32_t *)(ent + 28);
        c->is_dir      = (ent[11] & 0x10) != 0;
        c->ent_lba     = sec_lba;
        c->ent_off     = off_in_sec;
        c->found       = 1;
        return 1;
    }
    return 0;
}

/* ---------------- readdir ---------------- */

typedef struct {
    vfs_dirent_t *out;
    size_t        max;
    size_t        written;
} fat_readdir_ctx_t;

static int fat_readdir_cb(void *ctx, const uint8_t *ent, uint32_t sec_lba,
                          uint32_t off_in_sec, uint32_t cclust)
{
    fat_readdir_ctx_t *c = (fat_readdir_ctx_t *)ctx;
    char nm[16];
    (void)sec_lba; (void)off_in_sec; (void)cclust;
    if (c->written >= c->max)
        return 1;
    fat83_to_name(ent, nm);
    if (nm[0] == 0) return 0;
    strncpy(c->out[c->written].name, nm, sizeof(c->out[c->written].name) - 1);
    c->out[c->written].name[sizeof(c->out[c->written].name) - 1] = 0;
    c->out[c->written].type = (ent[11] & 0x10) ? S_IFDIR : S_IFREG;
    {
        uint16_t lo = *(uint16_t *)(ent + 26);
        uint16_t hi = *(uint16_t *)(ent + 20);
        c->out[c->written].ino = ((uint32_t)hi << 16) | lo;
    }
    c->written++;
    return 0;
}

/* ---------------- File read ---------------- */

static int fat_read_file(fat_node_t *n, void *buf, size_t count, off_t pos)
{
    fat_fs_t *fs = n->fs;
    uint8_t *out = (uint8_t *)buf;
    uint8_t *sec;
    uint32_t cl = n->first_clust;
    size_t done = 0;
    uint32_t skip = (uint32_t)pos;

    if ((uint64_t)pos >= n->size)
        return 0;
    if (count > n->size - (uint32_t)pos)
        count = n->size - (uint32_t)pos;

    sec = (uint8_t *)kmalloc(512);
    if (!sec) return -ENOMEM;

    while (cl >= 2 && cl < 0x0FFFFFF8u && done < count) {
        uint32_t i;
        for (i = 0; i < fs->sec_per_clust && done < count; i++) {
            uint32_t lba = clust_to_lba(fs, cl) + i;
            size_t off;
            if (fat_read_sector(fs, lba, sec) < 0)
                return -EIO;
            if (skip >= 512) { skip -= 512; continue; }
            off = skip; skip = 0;
            {
                size_t take = 512 - off;
                if (take > count - done) take = count - done;
                memcpy(out + done, sec + off, take);
                done += take;
            }
        }
        cl = fat_next(fs, cl);
    }
    return (int)done;
}

/* ---------------- File write (with cluster allocation) ---------------- */

static int fat_extend_chain(fat_node_t *n, uint32_t needed_clusters)
{
    fat_fs_t *fs = n->fs;
    uint32_t last;
    uint32_t existing = 0;

    if (n->first_clust >= 2 && n->first_clust < 0x0FFFFFF8u) {
        uint32_t cur = n->first_clust;
        last = cur;
        while (cur >= 2 && cur < 0x0FFFFFF8u) {
            existing++;
            last = cur;
            cur = fat_next(fs, cur);
        }
    } else {
        last = 0;
    }

    while (existing < needed_clusters) {
        uint32_t nc = fat_alloc_cluster(fs);
        if (!nc) return -ENOSPC;
        if (last == 0) {
            n->first_clust = nc;
        } else {
            if (fat_write_entry(fs, last, nc) < 0)
                return -EIO;
        }
        last = nc;
        existing++;
    }
    return 0;
}

static int fat_update_dir_entry(fat_node_t *n)
{
    fat_fs_t *fs = n->fs;
    uint8_t *sec;
    int rc;
    if (n->dir_ent_sector == 0)
        return 0; /* root directory has no entry to update */
    sec = (uint8_t *)kmalloc(512);
    if (!sec) return -ENOMEM;
    if (fat_read_sector(fs, n->dir_ent_sector, sec) < 0)
        return -EIO;
    *(uint16_t *)(sec + n->dir_ent_offset + 26) = (uint16_t)(n->first_clust & 0xFFFF);
    *(uint16_t *)(sec + n->dir_ent_offset + 20) = (uint16_t)((n->first_clust >> 16) & 0xFFFF);
    *(uint32_t *)(sec + n->dir_ent_offset + 28) = n->size;
    rc = fat_write_sector(fs, n->dir_ent_sector, sec);
    return rc;
}

static int fat_write_file(fat_node_t *n, const void *buf, size_t count, off_t pos)
{
    fat_fs_t *fs = n->fs;
    uint32_t new_size;
    uint32_t needed_clusters;
    uint32_t cluster_bytes;
    const uint8_t *src = (const uint8_t *)buf;
    size_t done = 0;
    uint8_t *sec;

    cluster_bytes = fs->bytes_per_sec * fs->sec_per_clust;
    new_size = (uint32_t)pos + count;
    if (new_size < n->size)
        new_size = n->size;
    needed_clusters = (new_size + cluster_bytes - 1) / cluster_bytes;
    if (needed_clusters == 0)
        needed_clusters = 1;

    if (fat_extend_chain(n, needed_clusters) < 0)
        return -ENOSPC;

    sec = (uint8_t *)kmalloc(512);
    if (!sec) return -ENOMEM;

    while (done < count) {
        uint32_t abs   = (uint32_t)pos + done;
        uint32_t which = abs / cluster_bytes;
        uint32_t in_cl = abs % cluster_bytes;
        uint32_t sec_no = in_cl / 512;
        uint32_t in_sec = in_cl % 512;
        uint32_t cl = n->first_clust;
        uint32_t k;
        uint32_t lba;
        size_t   chunk;

        for (k = 0; k < which; k++)
            cl = fat_next(fs, cl);
        if (cl < 2 || cl >= 0x0FFFFFF8u)
            break;
        lba = clust_to_lba(fs, cl) + sec_no;

        chunk = 512 - in_sec;
        if (chunk > count - done)
            chunk = count - done;

        if (in_sec != 0 || chunk != 512) {
            if (fat_read_sector(fs, lba, sec) < 0)
                return -EIO;
        }
        memcpy(sec + in_sec, src + done, chunk);
        if (fat_write_sector(fs, lba, sec) < 0)
            return -EIO;
        done += chunk;
    }

    if ((uint32_t)pos + done > n->size)
        n->size = (uint32_t)pos + done;
    (void)fat_update_dir_entry(n);
    return (int)done;
}

/* ---------------- VFS ops ---------------- */

static ssize_t fat_f_read(file_t *file, void *buf, size_t count, off_t *pos)
{
    fat_node_t *n = (fat_node_t *)file->f_inode->i_private;
    int rc;
    if (!n || n->is_dir) return -EISDIR;
    rc = fat_read_file(n, buf, count, *pos);
    if (rc < 0) return rc;
    *pos += rc;
    return rc;
}

static ssize_t fat_f_write(file_t *file, const void *buf, size_t count, off_t *pos)
{
    fat_node_t *n = (fat_node_t *)file->f_inode->i_private;
    int rc;
    if (!n || n->is_dir) return -EISDIR;
    rc = fat_write_file(n, buf, count, *pos);
    if (rc < 0) return rc;
    *pos += rc;
    file->f_inode->i_size = n->size;
    return rc;
}

static int fat_f_readdir(file_t *file, void *dirent, size_t max)
{
    fat_node_t *n = (fat_node_t *)file->f_inode->i_private;
    fat_readdir_ctx_t ctx;
    int rc;
    if (!n || !n->is_dir) return -ENOTDIR;
    ctx.out = (vfs_dirent_t *)dirent;
    ctx.max = max;
    ctx.written = 0;
    rc = fat_scan_dir(n->fs, n->first_clust, fat_readdir_cb, &ctx);
    if (rc < 0) return rc;
    return (int)ctx.written;
}

static dentry_t *fat_lookup(inode_t *dir, dentry_t *dentry)
{
    fat_node_t *dn = (fat_node_t *)dir->i_private;
    fat_lookup_ctx_t lc;
    fat_node_t *nn;
    inode_t *ino;
    dentry_t *out;

    if (!dn) return NULL;
    memset(&lc, 0, sizeof(lc));
    lc.want = dentry->d_name;
    if (fat_scan_dir(dn->fs, dn->first_clust, fat_lookup_cb, &lc) < 0)
        return NULL;
    if (!lc.found)
        return NULL;

    nn = (fat_node_t *)kmalloc(sizeof(*nn));
    if (!nn) return NULL;
    memset(nn, 0, sizeof(*nn));
    nn->fs = dn->fs;
    nn->first_clust = lc.first_clust;
    nn->size = lc.size;
    nn->is_dir = lc.is_dir;
    nn->dir_ent_sector = lc.ent_lba;
    nn->dir_ent_offset = lc.ent_off;

    ino = alloc_inode(dir->i_sb, lc.is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644));
    if (!ino) return NULL;
    ino->i_size = nn->size;
    ino->i_ino  = nn->first_clust;
    ino->i_private = nn;
    ino->i_op  = &fat_iops;
    ino->i_fop = &fat_fops;

    out = alloc_dentry(dentry->d_name, dentry->d_parent, ino);
    return out;
}

static const file_operations_t fat_fops = {
    .read    = fat_f_read,
    .write   = fat_f_write,
    .readdir = fat_f_readdir,
};

static const inode_operations_t fat_iops = {
    .lookup = fat_lookup,
};

/* ---------------- Mount ---------------- */

static int fat_mount(file_system_type_t *fs_type, int flags,
                     const char *dev_name, void *data, super_block_t **sb_out)
{
    const block_api_t *blk = block_api_get();
    block_device_t *bdev;
    uint8_t *bpb;
    fat_fs_t *fs;
    fat_node_t *root;
    inode_t *rino;
    dentry_t *rd;
    super_block_t *sb;
    uint16_t bps, rsvd, root_ents;
    uint8_t spc, nfats;
    uint32_t fatsz16, fatsz32, tot16, tot32, fatsz, totsec, data_secs, clusters;

    (void)flags;
    (void)data;
    if (!blk || !dev_name)
        return -EINVAL;
    bdev = blk->lookup(dev_name);
    if (!bdev)
        return -ENODEV;

    bpb = (uint8_t *)kmalloc(512);
    if (!bpb) return -ENOMEM;
    if (blk->read(bdev, 0, bpb, 1) < 0)
        return -EIO;
    if (bpb[510] != 0x55 || bpb[511] != 0xAA)
        return -EINVAL;

    bps = *(uint16_t *)(bpb + 11);
    spc = bpb[13];
    rsvd = *(uint16_t *)(bpb + 14);
    nfats = bpb[16];
    root_ents = *(uint16_t *)(bpb + 17);
    tot16 = *(uint16_t *)(bpb + 19);
    fatsz16 = *(uint16_t *)(bpb + 22);
    tot32 = *(uint32_t *)(bpb + 32);
    fatsz32 = *(uint32_t *)(bpb + 36);

    if (bps != 512 || spc == 0 || nfats == 0)
        return -EINVAL;

    fatsz  = fatsz16 ? fatsz16 : fatsz32;
    totsec = tot16 ? tot16 : tot32;
    data_secs = totsec - (rsvd + nfats * fatsz +
                          ((root_ents * 32) + (bps - 1)) / bps);
    clusters = data_secs / spc;

    fs = (fat_fs_t *)kmalloc(sizeof(*fs));
    if (!fs) return -ENOMEM;
    memset(fs, 0, sizeof(*fs));
    fs->bdev = bdev;
    fs->bytes_per_sec = bps;
    fs->sec_per_clust = spc;
    fs->reserved = rsvd;
    fs->fats = nfats;
    fs->fat_sectors = fatsz;
    fs->root_ents = root_ents;
    fs->clusters = clusters;
    if (clusters < 4085)      fs->fat_bits = 12;
    else if (clusters < 65525) fs->fat_bits = 16;
    else                       fs->fat_bits = 32;
    fs->root_clust = fs->fat_bits == 32 ? *(uint32_t *)(bpb + 44) : 0;
    fs->data_start = rsvd + nfats * fatsz +
                     ((root_ents * 32) + (bps - 1)) / bps;

    root = (fat_node_t *)kmalloc(sizeof(*root));
    sb   = (super_block_t *)kmalloc(sizeof(*sb));
    if (!root || !sb) return -ENOMEM;
    memset(root, 0, sizeof(*root));
    memset(sb, 0, sizeof(*sb));
    root->fs = fs;
    root->first_clust = fs->root_clust;
    root->is_dir = 1;

    rino = alloc_inode(sb, S_IFDIR | 0755);
    if (!rino) return -ENOMEM;
    rino->i_op = &fat_iops;
    rino->i_fop = &fat_fops;
    rino->i_private = root;

    rd = alloc_dentry("/", NULL, rino);
    if (!rd) return -ENOMEM;

    sb->s_type = fs_type;
    sb->s_root = rd;
    sb->s_bdev = bdev;
    sb->s_fs_info = fs;
    *sb_out = sb;

    vga_print("fat: mounted ");
    vga_print(dev_name);
    vga_print(" (fat");
    vga_print_uint((uint32_t)fs->fat_bits);
    vga_print(")\n");
    return 0;
}

static file_system_type_t g_fat = {
    .name = "fat",
    .mount = fat_mount,
};

static file_system_type_t g_vfat = {
    .name = "vfat",
    .mount = fat_mount,
};

static int fat_init(driver_t *drv, void *ctx)
{
    const vfs_api_t *api = vfs_api_get();
    (void)drv;
    (void)ctx;
    if (!api || !api->register_filesystem)
        return -1;
    if (!api->alloc_inode || !api->alloc_dentry)
        return -1;
    alloc_inode = (inode_t *(*)(super_block_t *, uint32_t))api->alloc_inode;
    alloc_dentry = (dentry_t *(*)(const char *, dentry_t *, inode_t *))api->alloc_dentry;
    if (api->register_filesystem(&g_fat) < 0)
        return -1;
    (void)api->register_filesystem(&g_vfat);
    vga_print("fat: registered\n");
    return 0;
}

int kmod_init(void)
{
    driver_t d;
    memset(&d, 0, sizeof(d));
    strncpy(d.name, "fat", DRIVER_NAME_MAX - 1);
    strncpy(d.version, "1.1", DRIVER_VERSION_MAX - 1);
    d.kind = DRIVER_KIND_CUSTOM;
    d.class = DRIVER_CLASS_FS;
    d.priority = 60;
    d.init = fat_init;
    if (driver_register(&d) < 0)
        return -1;
    if (driver_load("fat", NULL) < 0)
        return -1;
    return 0;
}
