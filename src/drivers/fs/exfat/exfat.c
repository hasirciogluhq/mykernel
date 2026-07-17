/*
 * exFAT read-only filesystem driver.
 *
 * Parses the Main Boot Sector to determine cluster/FAT layout, then
 * walks the root directory chain looking for File (0x85) directory
 * entry sets.  A complete file entry set is: one 0x85 primary, one
 * 0xC0 Stream Extension, and one or more 0xC1 File Name entries
 * carrying UTF-16LE code points (which we down-convert to ASCII for
 * comparison purposes).
 *
 * Cluster runs are traversed either contiguously (NoFatChain flag)
 * or by following the 32-bit FAT.  Reads support arbitrary offsets
 * within a chained file.
 *
 * Everything is little-endian on disk; the target is x86 so we do
 * light `rd_leXX` helpers for clarity.
 */

#include <kernel/vfs_api.h>
#include <kernel/block_api.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/string.h>
#include <drivers/vfs_fs.h>
#include <drivers/driver.h>
#include <drivers/vga.h>

#define EXFAT_ENTRY_FILE   0x85u
#define EXFAT_ENTRY_STREAM 0xC0u
#define EXFAT_ENTRY_NAME   0xC1u
#define EXFAT_ENTRY_EOD    0x00u
#define EXFAT_ENTRY_INUSE  0x80u   /* high bit means "in use" */

#define EXFAT_ATTR_DIRECTORY 0x0010

#define EXFAT_EOF_CLUSTER    0xFFFFFFFFu
#define EXFAT_BAD_CLUSTER    0xFFFFFFF7u
#define EXFAT_LAST_VALID     0xFFFFFFF6u

typedef struct {
    block_device_t *bdev;

    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t bytes_per_cluster;
    uint32_t sec_per_cluster_shift;
    uint32_t bytes_per_sec_shift;

    uint64_t fat_offset;     /* in sectors */
    uint64_t fat_length;     /* sectors of one FAT */
    uint64_t cluster_heap;   /* in sectors */
    uint32_t cluster_count;
    uint32_t root_cluster;

    uint32_t sec_per_cluster_512; /* 512-byte sectors per cluster (for block API) */
} exfat_fs_t;

typedef struct {
    exfat_fs_t *fs;
    uint32_t    first_cluster;
    uint64_t    size;
    int         no_fat_chain;
    int         is_dir;
} exfat_node_t;

static dentry_t *(*alloc_dentry)(const char *, dentry_t *, inode_t *);
static inode_t  *(*alloc_inode)(super_block_t *, uint32_t);

static const inode_operations_t exfat_iops;
static const file_operations_t  exfat_fops;

/* ---------------- LE helpers ---------------- */

static inline uint16_t rd_le16(const void *p)
{
    const uint8_t *b = (const uint8_t *)p;
    return (uint16_t)(b[0] | (b[1] << 8));
}
static inline uint32_t rd_le32(const void *p)
{
    const uint8_t *b = (const uint8_t *)p;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static inline uint64_t rd_le64(const void *p)
{
    return (uint64_t)rd_le32(p) | ((uint64_t)rd_le32((const uint8_t *)p + 4) << 32);
}

/* ---------------- Block I/O ---------------- */

static int exfat_read_sectors(exfat_fs_t *fs, uint64_t sec, void *buf, uint32_t count)
{
    const block_api_t *api = block_api_get();
    uint64_t lba;
    uint32_t nsect;
    if (!api || !api->read)
        return -EIO;
    lba   = sec * (fs->bytes_per_sector / 512u);
    nsect = count * (fs->bytes_per_sector / 512u);
    return api->read(fs->bdev, lba, buf, nsect);
}

static int exfat_read_cluster(exfat_fs_t *fs, uint32_t cluster, void *buf)
{
    uint64_t sec;
    if (cluster < 2)
        return -EINVAL;
    sec = fs->cluster_heap + (uint64_t)(cluster - 2) * fs->sectors_per_cluster;
    return exfat_read_sectors(fs, sec, buf, fs->sectors_per_cluster);
}

/* ---------------- FAT walk ---------------- */

static uint32_t exfat_next_cluster(exfat_fs_t *fs, uint32_t cluster)
{
    uint8_t *sec;
    uint64_t byte_off = (uint64_t)cluster * 4;
    uint64_t sec_no   = fs->fat_offset + (byte_off / fs->bytes_per_sector);
    uint32_t in_sec   = (uint32_t)(byte_off % fs->bytes_per_sector);
    uint32_t out;

    sec = (uint8_t *)kmalloc(fs->bytes_per_sector);
    if (!sec)
        return EXFAT_EOF_CLUSTER;
    if (exfat_read_sectors(fs, sec_no, sec, 1) < 0)
        return EXFAT_EOF_CLUSTER;
    out = rd_le32(sec + in_sec);
    return out;
}

/* ---------------- Cluster iteration ---------------- */

typedef int (*exfat_cluster_cb)(void *ctx, uint32_t cluster, uint32_t index);

static int exfat_walk_chain(exfat_fs_t *fs, uint32_t start, int no_fat_chain,
                            uint32_t max_clusters,
                            exfat_cluster_cb cb, void *ctx)
{
    uint32_t cur = start;
    uint32_t i;

    if (start < 2)
        return -EINVAL;

    if (no_fat_chain) {
        for (i = 0; i < max_clusters; i++) {
            int rc = cb(ctx, start + i, i);
            if (rc != 0)
                return rc;
        }
        return 0;
    }
    i = 0;
    while (cur >= 2 && cur <= EXFAT_LAST_VALID && i < max_clusters) {
        int rc = cb(ctx, cur, i);
        if (rc != 0)
            return rc;
        cur = exfat_next_cluster(fs, cur);
        i++;
    }
    return 0;
}

/* ---------------- UTF-16LE → ASCII (best effort, folded to lowercase) ---------------- */

static void utf16_to_ascii(const uint8_t *u16, int chars, char *out, int *out_len)
{
    int i;
    int j = 0;
    for (i = 0; i < chars; i++) {
        uint16_t cp = rd_le16(u16 + i * 2);
        if (cp == 0)
            break;
        if (cp < 0x80)
            out[j++] = (char)cp;
        else
            out[j++] = '?';
    }
    out[j] = 0;
    *out_len = j;
}

static int name_cieq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* ---------------- Directory iteration ---------------- */

typedef struct {
    char     name[256];
    uint32_t first_cluster;
    uint64_t size;
    uint16_t attributes;
    uint8_t  secondary_flags;
    int      valid;
} exfat_entry_t;

typedef int (*exfat_dir_cb)(void *ctx, const exfat_entry_t *e);

static int exfat_iterate_dir(exfat_node_t *dir, exfat_dir_cb cb, void *ctx)
{
    exfat_fs_t *fs = dir->fs;
    uint8_t *cluster_buf;
    uint32_t cur = dir->first_cluster;
    int no_chain = dir->no_fat_chain;

    if (cur < 2)
        return -EINVAL;

    cluster_buf = (uint8_t *)kmalloc(fs->bytes_per_cluster);
    if (!cluster_buf)
        return -ENOMEM;

    while (cur >= 2 && cur <= EXFAT_LAST_VALID) {
        uint32_t off = 0;
        if (exfat_read_cluster(fs, cur, cluster_buf) < 0)
            return -EIO;
        while (off + 32 <= fs->bytes_per_cluster) {
            uint8_t type = cluster_buf[off];
            if (type == EXFAT_ENTRY_EOD)
                return 0;
            if ((type & EXFAT_ENTRY_INUSE) == 0) {
                off += 32;
                continue;
            }
            if (type == EXFAT_ENTRY_FILE) {
                uint8_t secondary_count = cluster_buf[off + 1];
                uint16_t attributes = rd_le16(cluster_buf + off + 4);
                uint32_t needed = (secondary_count + 1) * 32u;
                exfat_entry_t e;
                int total_name = 0;
                int have_stream = 0;

                if (off + needed > fs->bytes_per_cluster) {
                    /* Entry set crosses cluster boundary — treat as
                     * end for simplicity. Real drivers can span here. */
                    off = fs->bytes_per_cluster;
                    continue;
                }
                memset(&e, 0, sizeof(e));
                e.attributes = attributes;

                /* Iterate the secondary entries. */
                {
                    uint32_t sub_off = off + 32;
                    uint32_t remaining = secondary_count;
                    uint32_t name_slots = 0;
                    while (remaining--) {
                        uint8_t t = cluster_buf[sub_off];
                        if (t == EXFAT_ENTRY_STREAM) {
                            e.secondary_flags = cluster_buf[sub_off + 1];
                            /* NameLength at offset 3, cluster at 20, size at 24. */
                            total_name = cluster_buf[sub_off + 3];
                            e.first_cluster = rd_le32(cluster_buf + sub_off + 20);
                            e.size          = rd_le64(cluster_buf + sub_off + 24);
                            have_stream = 1;
                        } else if (t == EXFAT_ENTRY_NAME) {
                            uint32_t chars_this = 15;
                            uint32_t taken = name_slots * 15;
                            int need = total_name - (int)taken;
                            if (need < 0) need = 0;
                            if ((int)chars_this > need)
                                chars_this = (uint32_t)need;
                            if (chars_this) {
                                char part[64];
                                int part_len = 0;
                                utf16_to_ascii(cluster_buf + sub_off + 2,
                                               (int)chars_this, part, &part_len);
                                {
                                    int have = 0;
                                    while (e.name[have]) have++;
                                    if (have + part_len < (int)sizeof(e.name))
                                        memcpy(e.name + have, part, (size_t)part_len + 1);
                                }
                                name_slots++;
                            }
                        }
                        sub_off += 32;
                    }
                }

                if (have_stream) {
                    e.valid = 1;
                    {
                        int rc = cb(ctx, &e);
                        if (rc) return rc;
                    }
                }
                off += needed;
                continue;
            }
            off += 32;
        }
        if (no_chain) {
            cur++;
            /* No easy "chain length" for the root without reading a
             * bitmap; the caller (root directory) uses chained walk
             * with FAT. We just fall through and stop when we hit an
             * EOD entry. */
            if (dir->size && (uint64_t)((cur - dir->first_cluster) * fs->bytes_per_cluster) >= dir->size)
                break;
        } else {
            cur = exfat_next_cluster(fs, cur);
        }
    }
    return 0;
}

typedef struct {
    const char *want;
    exfat_entry_t match;
    int found;
} exfat_lookup_ctx_t;

static int exfat_lookup_cb(void *ctx, const exfat_entry_t *e)
{
    exfat_lookup_ctx_t *c = (exfat_lookup_ctx_t *)ctx;
    if (name_cieq(e->name, c->want)) {
        c->match = *e;
        c->found = 1;
        return 1;
    }
    return 0;
}

typedef struct {
    vfs_dirent_t *out;
    size_t        max;
    size_t        written;
} exfat_readdir_ctx_t;

static int exfat_readdir_cb(void *ctx, const exfat_entry_t *e)
{
    exfat_readdir_ctx_t *c = (exfat_readdir_ctx_t *)ctx;
    if (c->written >= c->max)
        return 1;
    strncpy(c->out[c->written].name, e->name, sizeof(c->out[c->written].name) - 1);
    c->out[c->written].name[sizeof(c->out[c->written].name) - 1] = 0;
    c->out[c->written].type = (e->attributes & EXFAT_ATTR_DIRECTORY) ? S_IFDIR : S_IFREG;
    c->out[c->written].ino  = e->first_cluster;
    c->written++;
    return 0;
}

/* ---------------- File read ---------------- */

typedef struct {
    uint8_t *out;
    exfat_fs_t *fs;
    uint32_t bytes_per_cluster;
    uint64_t start_off;
    uint64_t end_off;
    uint64_t done;
    uint8_t *cluster_buf;
} exfat_read_ctx_t;

static int exfat_read_cluster_cb(void *ctx, uint32_t cluster, uint32_t index)
{
    exfat_read_ctx_t *r = (exfat_read_ctx_t *)ctx;
    uint64_t cluster_begin = (uint64_t)index * r->bytes_per_cluster;
    uint64_t cluster_end   = cluster_begin + r->bytes_per_cluster;
    uint64_t copy_from;
    uint64_t copy_to;
    uint32_t src_off;
    uint32_t chunk;

    if (cluster_end <= r->start_off)
        return 0;
    if (cluster_begin >= r->end_off)
        return 1;

    if (exfat_read_cluster(r->fs, cluster, r->cluster_buf) < 0)
        return -EIO;

    copy_from = r->start_off > cluster_begin ? r->start_off : cluster_begin;
    copy_to   = r->end_off   < cluster_end   ? r->end_off   : cluster_end;
    src_off   = (uint32_t)(copy_from - cluster_begin);
    chunk     = (uint32_t)(copy_to - copy_from);
    memcpy(r->out + r->done, r->cluster_buf + src_off, chunk);
    r->done += chunk;
    return 0;
}

static int exfat_read_file(exfat_node_t *n, void *buf, size_t count, off_t pos)
{
    exfat_read_ctx_t rctx;
    uint32_t max_clusters;

    if ((uint64_t)pos >= n->size)
        return 0;
    if ((uint64_t)pos + count > n->size)
        count = (size_t)(n->size - (uint64_t)pos);

    rctx.fs = n->fs;
    rctx.bytes_per_cluster = n->fs->bytes_per_cluster;
    rctx.out = (uint8_t *)buf;
    rctx.start_off = (uint64_t)pos;
    rctx.end_off   = (uint64_t)pos + count;
    rctx.done = 0;
    rctx.cluster_buf = (uint8_t *)kmalloc(rctx.bytes_per_cluster);
    if (!rctx.cluster_buf)
        return -ENOMEM;

    max_clusters = (uint32_t)((n->size + rctx.bytes_per_cluster - 1) /
                              rctx.bytes_per_cluster);
    if (exfat_walk_chain(n->fs, n->first_cluster, n->no_fat_chain,
                         max_clusters, exfat_read_cluster_cb, &rctx) < 0)
        return -EIO;
    return (int)rctx.done;
}

/* ---------------- VFS ops ---------------- */

static ssize_t exfat_f_read(file_t *file, void *buf, size_t count, off_t *pos)
{
    exfat_node_t *n = (exfat_node_t *)file->f_inode->i_private;
    int rc;
    if (!n) return -EIO;
    if (n->is_dir) return -EISDIR;
    rc = exfat_read_file(n, buf, count, *pos);
    if (rc < 0) return rc;
    *pos += rc;
    return rc;
}

static int exfat_f_readdir(file_t *file, void *dirent, size_t max)
{
    exfat_node_t *n = (exfat_node_t *)file->f_inode->i_private;
    exfat_readdir_ctx_t ctx;
    int rc;
    if (!n || !n->is_dir)
        return -ENOTDIR;
    ctx.out = (vfs_dirent_t *)dirent;
    ctx.max = max;
    ctx.written = 0;
    rc = exfat_iterate_dir(n, exfat_readdir_cb, &ctx);
    if (rc < 0)
        return rc;
    return (int)ctx.written;
}

static dentry_t *exfat_lookup(inode_t *dir, dentry_t *dentry)
{
    exfat_node_t *dn = (exfat_node_t *)dir->i_private;
    exfat_lookup_ctx_t lc;
    exfat_node_t *child;
    inode_t *ino;
    dentry_t *out;

    if (!dn || !dn->is_dir)
        return NULL;
    memset(&lc, 0, sizeof(lc));
    lc.want = dentry->d_name;
    if (exfat_iterate_dir(dn, exfat_lookup_cb, &lc) < 0)
        return NULL;
    if (!lc.found)
        return NULL;

    child = (exfat_node_t *)kmalloc(sizeof(*child));
    if (!child)
        return NULL;
    memset(child, 0, sizeof(*child));
    child->fs = dn->fs;
    child->first_cluster = lc.match.first_cluster;
    child->size          = lc.match.size;
    child->is_dir        = (lc.match.attributes & EXFAT_ATTR_DIRECTORY) ? 1 : 0;
    child->no_fat_chain  = (lc.match.secondary_flags & 0x02) ? 1 : 0;

    ino = alloc_inode(dir->i_sb, child->is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644));
    if (!ino)
        return NULL;
    ino->i_size    = child->size;
    ino->i_ino     = child->first_cluster;
    ino->i_private = child;
    ino->i_op      = &exfat_iops;
    ino->i_fop     = &exfat_fops;

    out = alloc_dentry(dentry->d_name, dentry->d_parent, ino);
    return out;
}

static const inode_operations_t exfat_iops = {
    .lookup = exfat_lookup,
};

static const file_operations_t exfat_fops = {
    .read    = exfat_f_read,
    .readdir = exfat_f_readdir,
};

/* ---------------- Mount ---------------- */

static int exfat_mount(file_system_type_t *fs_type, int flags,
                       const char *dev_name, void *data, super_block_t **sb_out)
{
    const block_api_t *blk = block_api_get();
    block_device_t *bdev;
    uint8_t *boot;
    exfat_fs_t *fs;
    exfat_node_t *root;
    inode_t *rino;
    dentry_t *rd;
    super_block_t *sb;

    (void)flags; (void)data;
    if (!blk || !dev_name)
        return -EINVAL;
    bdev = blk->lookup(dev_name);
    if (!bdev)
        return -ENODEV;

    /* Read 512 bytes first, that's enough to inspect the fixed header. */
    boot = (uint8_t *)kmalloc(512);
    if (!boot)
        return -ENOMEM;
    if (blk->read(bdev, 0, boot, 1) < 0)
        return -EIO;
    if (memcmp(boot + 3, "EXFAT   ", 8) != 0)
        return -EINVAL;

    fs = (exfat_fs_t *)kmalloc(sizeof(*fs));
    if (!fs)
        return -ENOMEM;
    memset(fs, 0, sizeof(*fs));
    fs->bdev = bdev;
    fs->fat_offset          = rd_le32(boot + 80);
    fs->fat_length          = rd_le32(boot + 84);
    fs->cluster_heap        = rd_le32(boot + 88);
    fs->cluster_count       = rd_le32(boot + 92);
    fs->root_cluster        = rd_le32(boot + 96);
    fs->bytes_per_sec_shift = boot[108];
    fs->sec_per_cluster_shift = boot[109];
    fs->bytes_per_sector    = 1u << fs->bytes_per_sec_shift;
    fs->sectors_per_cluster = 1u << fs->sec_per_cluster_shift;
    fs->bytes_per_cluster   = fs->bytes_per_sector * fs->sectors_per_cluster;
    fs->sec_per_cluster_512 = fs->bytes_per_cluster / 512u;

    if (fs->bytes_per_sector < 512 || fs->bytes_per_cluster < 512)
        return -EINVAL;
    if (fs->root_cluster < 2)
        return -EINVAL;

    root = (exfat_node_t *)kmalloc(sizeof(*root));
    sb   = (super_block_t *)kmalloc(sizeof(*sb));
    if (!root || !sb)
        return -ENOMEM;
    memset(root, 0, sizeof(*root));
    memset(sb, 0, sizeof(*sb));
    root->fs = fs;
    root->first_cluster = fs->root_cluster;
    root->is_dir = 1;
    root->no_fat_chain = 0; /* root uses FAT chain */
    /* Root size is unknown up front; iterate until EOD entry. */
    root->size = 0;

    rino = alloc_inode(sb, S_IFDIR | 0755);
    if (!rino)
        return -ENOMEM;
    rino->i_ino     = fs->root_cluster;
    rino->i_private = root;
    rino->i_op      = &exfat_iops;
    rino->i_fop     = &exfat_fops;

    rd = alloc_dentry("/", NULL, rino);
    if (!rd)
        return -ENOMEM;

    sb->s_type    = fs_type;
    sb->s_root    = rd;
    sb->s_bdev    = bdev;
    sb->s_fs_info = fs;
    *sb_out       = sb;

    vga_print("exfat: mounted ");
    vga_print(dev_name);
    vga_print(" (bpc=");
    vga_print_uint(fs->bytes_per_cluster);
    vga_print(")\n");
    return 0;
}

/* ---------------- Registration ---------------- */

static file_system_type_t g_fs = {
    .name  = "exfat",
    .mount = exfat_mount,
};

static int exfat_init(driver_t *drv, void *ctx)
{
    const vfs_api_t *api = vfs_api_get();
    (void)drv; (void)ctx;
    if (!api || !api->register_filesystem)
        return -1;
    if (!api->alloc_inode || !api->alloc_dentry)
        return -1;
    alloc_inode  = (inode_t *(*)(super_block_t *, uint32_t))api->alloc_inode;
    alloc_dentry = (dentry_t *(*)(const char *, dentry_t *, inode_t *))api->alloc_dentry;
    if (api->register_filesystem(&g_fs) < 0)
        return -1;
    vga_print("exfat: registered\n");
    return 0;
}

int kmod_init(void)
{
    driver_t d;
    memset(&d, 0, sizeof(d));
    strncpy(d.name,    "exfat", DRIVER_NAME_MAX    - 1);
    strncpy(d.version, "1.0",   DRIVER_VERSION_MAX - 1);
    d.kind     = DRIVER_KIND_CUSTOM;
    d.class    = DRIVER_CLASS_FS;
    d.priority = 65;
    d.init     = exfat_init;
    if (driver_register(&d) < 0)
        return -1;
    if (driver_load("exfat", NULL) < 0)
        return -1;
    return 0;
}
