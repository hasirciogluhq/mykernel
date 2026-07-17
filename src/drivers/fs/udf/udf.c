/*
 * UDF (ECMA-167 / OSTA UDF) read-only filesystem driver.
 *
 * Boot / mount flow:
 *   1. Read sector 256 → Anchor Volume Descriptor Pointer (tag id 2).
 *   2. Walk the Main Volume Descriptor Sequence looking for the
 *      Partition Descriptor (tag id 5) and Logical Volume Descriptor
 *      (tag id 6).  Extract the partition start LBN and the fileset
 *      long_ad from the LVD.
 *   3. Follow the fileset long_ad to the File Set Descriptor (256),
 *      which carries the long_ad of the root ICB.
 *   4. Follow the root ICB to a File Entry (261) or Extended File
 *      Entry (266) describing the root directory.
 *   5. Directory listing = iterate File Identifier Descriptors (257)
 *      stored in the file entry's data extents.
 *
 * Only 2048-byte logical blocks and short_ad / long_ad allocation
 * descriptors are supported (the standard configuration for optical
 * media and virtually all UDF images).  Embedded data (allocation
 * type 3) is handled for small files.
 */

#include <kernel/vfs_api.h>
#include <kernel/block_api.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/string.h>
#include <drivers/vfs_fs.h>
#include <drivers/driver.h>
#include <drivers/vga.h>

#define UDF_LB_SIZE          2048u
#define UDF_LB_TO_SECT       (UDF_LB_SIZE / 512u)
#define UDF_ANCHOR_LBN       256u

/* Descriptor tag identifiers */
#define UDF_TAG_PVD          1u
#define UDF_TAG_AVDP         2u
#define UDF_TAG_VDP          3u
#define UDF_TAG_IUVD         4u
#define UDF_TAG_PART_DESC    5u
#define UDF_TAG_LVD          6u
#define UDF_TAG_USD          7u
#define UDF_TAG_TERM         8u
#define UDF_TAG_LVID         9u
#define UDF_TAG_FSD          256u
#define UDF_TAG_FID          257u
#define UDF_TAG_ALLOC_EXT    258u
#define UDF_TAG_INDIRECT     259u
#define UDF_TAG_TERM_ENTRY   260u
#define UDF_TAG_FILE_ENTRY   261u
#define UDF_TAG_EXT_FE       266u

/* File characteristics in FID */
#define UDF_FID_HIDDEN       0x01
#define UDF_FID_DIRECTORY    0x02
#define UDF_FID_DELETED      0x04
#define UDF_FID_PARENT       0x08

/* File type in ICBTag */
#define UDF_FILE_TYPE_DIR    4
#define UDF_FILE_TYPE_REG    5
#define UDF_FILE_TYPE_SYMLINK 12

/* ICBTag flags: allocation type in bits 0..2 */
#define UDF_ICB_ALLOC_SHORT   0
#define UDF_ICB_ALLOC_LONG    1
#define UDF_ICB_ALLOC_EXT     2
#define UDF_ICB_ALLOC_EMBED   3

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

/* ---------------- Data types ---------------- */

typedef struct {
    uint32_t block;
    uint16_t partition;
} udf_lb_addr_t;

typedef struct {
    uint32_t      length_flags; /* top 2 bits type, bottom 30 length */
    udf_lb_addr_t location;
} udf_long_ad_t;

typedef struct {
    uint32_t length_flags;
    uint32_t position;  /* logical block number in partition */
} udf_short_ad_t;

typedef struct {
    block_device_t *bdev;
    uint32_t partition_start;   /* absolute LBN */
    uint32_t partition_length;
    uint32_t root_icb_block;    /* in partition */
    uint16_t root_icb_partition;
    uint32_t root_icb_length;
} udf_fs_t;

/* Simplified extent record: keeps physical LBN + length in bytes. */
typedef struct {
    uint32_t phys_lbn;   /* absolute logical block number on disk */
    uint32_t bytes;
    int      sparse;
    int      unwritten;
} udf_extent_t;

typedef struct {
    udf_extent_t *extents;
    int           nextents;
    uint64_t      size;

    /* embedded content for small files (alloc type 3) */
    uint8_t      *embedded;
    uint32_t      embedded_size;
} udf_content_t;

typedef struct {
    udf_fs_t     *fs;
    uint32_t      icb_block;    /* absolute LBN of the FE */
    uint64_t      size;
    int           is_dir;
    udf_content_t data;
} udf_node_t;

static dentry_t *(*alloc_dentry)(const char *, dentry_t *, inode_t *);
static inode_t  *(*alloc_inode)(super_block_t *, uint32_t);

static const inode_operations_t udf_iops;
static const file_operations_t  udf_fops;

/* ---------------- Block I/O ---------------- */

static int udf_read_lb(udf_fs_t *fs, uint32_t abs_lbn, void *buf)
{
    const block_api_t *api = block_api_get();
    if (!api || !api->read)
        return -EIO;
    return api->read(fs->bdev, (uint64_t)abs_lbn * UDF_LB_TO_SECT, buf, UDF_LB_TO_SECT);
}

/* Convert (partition, lbn) → absolute LBN. */
static uint32_t udf_resolve_lbn(udf_fs_t *fs, uint32_t lbn, uint16_t partition)
{
    (void)partition;
    return fs->partition_start + lbn;
}

/* ---------------- Long-AD / short-AD parsing ---------------- */

static void udf_parse_long_ad(const uint8_t *p, udf_long_ad_t *out)
{
    out->length_flags       = rd_le32(p + 0);
    out->location.block     = rd_le32(p + 4);
    out->location.partition = rd_le16(p + 8);
}

static void udf_parse_short_ad(const uint8_t *p, udf_short_ad_t *out)
{
    out->length_flags = rd_le32(p + 0);
    out->position     = rd_le32(p + 4);
}

/* ---------------- Content parsing (FE / EFE) ---------------- */

static int udf_parse_alloc_descs(udf_fs_t *fs, const uint8_t *fe, uint32_t fe_size,
                                 int alloc_type, uint32_t l_ea, uint32_t l_ad,
                                 udf_content_t *out)
{
    uint32_t base = 176u + l_ea;   /* FE fixed header size + EAs */
    /* Extended File Entry (266) has additional 40 bytes before EAs. */
    /* Caller can override via base_override, but we detect by tag id externally. */
    (void)fe_size;

    if (alloc_type == UDF_ICB_ALLOC_EMBED) {
        out->embedded = (uint8_t *)kmalloc(l_ad ? l_ad : 1);
        if (!out->embedded)
            return -ENOMEM;
        memcpy(out->embedded, fe + base, l_ad);
        out->embedded_size = l_ad;
        return 0;
    }

    {
        uint32_t off = 0;
        int      cap = 8;
        int      cnt = 0;
        out->extents = (udf_extent_t *)kmalloc(sizeof(udf_extent_t) * cap);
        if (!out->extents)
            return -ENOMEM;

        while (off < l_ad) {
            udf_extent_t e;
            uint32_t     lf;
            uint32_t     len;
            uint32_t     etype;
            uint32_t     part;
            uint32_t     blk;
            uint32_t     adsize;

            if (alloc_type == UDF_ICB_ALLOC_SHORT) {
                udf_short_ad_t sa;
                udf_parse_short_ad(fe + base + off, &sa);
                lf = sa.length_flags;
                blk = sa.position;
                part = 0;
                adsize = 8;
            } else if (alloc_type == UDF_ICB_ALLOC_LONG) {
                udf_long_ad_t la;
                udf_parse_long_ad(fe + base + off, &la);
                lf = la.length_flags;
                blk = la.location.block;
                part = la.location.partition;
                adsize = 16;
            } else {
                break; /* ExtAD not supported */
            }

            len   = lf & 0x3FFFFFFFu;
            etype = (lf >> 30) & 0x3u;
            if (len == 0)
                break;

            memset(&e, 0, sizeof(e));
            e.bytes = len;
            e.phys_lbn = udf_resolve_lbn(fs, blk, (uint16_t)part);
            if (etype == 2)
                e.sparse = 1;
            else if (etype == 1)
                e.unwritten = 1;
            /* etype == 3 means "next extent of ADs" — not supported (rare). */
            if (etype == 3) {
                /* Skip; a real driver follows this pointer. */
                off += adsize;
                continue;
            }

            if (cnt >= cap) {
                int newcap = cap * 2;
                udf_extent_t *nx = (udf_extent_t *)kmalloc(sizeof(udf_extent_t) * newcap);
                if (!nx)
                    return -ENOMEM;
                memcpy(nx, out->extents, sizeof(udf_extent_t) * cap);
                out->extents = nx;
                cap = newcap;
            }
            out->extents[cnt++] = e;
            off += adsize;
        }
        out->nextents = cnt;
    }
    return 0;
}

static int udf_read_content(udf_fs_t *fs, const udf_content_t *c,
                            uint64_t offset, uint32_t bytes, uint8_t *out)
{
    uint32_t done = 0;
    uint8_t *blk;

    if (c->embedded) {
        if (offset >= c->embedded_size)
            return 0;
        if (offset + bytes > c->embedded_size)
            bytes = c->embedded_size - (uint32_t)offset;
        memcpy(out, c->embedded + offset, bytes);
        return (int)bytes;
    }

    if (offset >= c->size)
        return 0;
    if (offset + bytes > c->size)
        bytes = (uint32_t)(c->size - offset);

    blk = (uint8_t *)kmalloc(UDF_LB_SIZE);
    if (!blk)
        return -ENOMEM;

    {
        uint64_t cursor = 0;
        int i;
        for (i = 0; i < c->nextents && done < bytes; i++) {
            const udf_extent_t *e = &c->extents[i];
            uint64_t e_start = cursor;
            uint64_t e_end   = cursor + e->bytes;
            if (e_end > offset && e_start < offset + bytes) {
                uint64_t seg_start = e_start > offset ? e_start : offset;
                uint64_t seg_end   = e_end   < (offset + bytes) ? e_end : (offset + bytes);
                uint64_t seg_off_in_ext = seg_start - e_start;
                uint32_t seg_len = (uint32_t)(seg_end - seg_start);
                if (e->sparse) {
                    memset(out + done, 0, seg_len);
                } else {
                    uint32_t bytes_left = seg_len;
                    uint32_t buf_off    = 0;
                    while (bytes_left) {
                        uint32_t block_idx = (uint32_t)(seg_off_in_ext + buf_off) / UDF_LB_SIZE;
                        uint32_t block_off = (uint32_t)(seg_off_in_ext + buf_off) % UDF_LB_SIZE;
                        uint32_t chunk = UDF_LB_SIZE - block_off;
                        if (chunk > bytes_left)
                            chunk = bytes_left;
                        if (udf_read_lb(fs, e->phys_lbn + block_idx, blk) < 0)
                            return -EIO;
                        memcpy(out + done + buf_off, blk + block_off, chunk);
                        buf_off += chunk;
                        bytes_left -= chunk;
                    }
                }
                done += seg_len;
            }
            cursor = e_end;
        }
    }
    return (int)done;
}

/* ---------------- Read FE/EFE and build a udf_node ---------------- */

static int udf_load_node(udf_fs_t *fs, uint32_t abs_lbn, udf_node_t *out)
{
    uint8_t *fe = (uint8_t *)kmalloc(UDF_LB_SIZE);
    uint16_t tag_id;
    uint8_t  file_type;
    uint16_t icb_flags;
    uint32_t l_ea, l_ad;
    uint32_t info_length_off;
    uint32_t base_offset;
    int      alloc_type;

    if (!fe) return -ENOMEM;
    if (udf_read_lb(fs, abs_lbn, fe) < 0)
        return -EIO;
    tag_id = rd_le16(fe + 0);
    if (tag_id != UDF_TAG_FILE_ENTRY && tag_id != UDF_TAG_EXT_FE)
        return -EINVAL;

    /* ICB Tag starts at offset 16 (immediately after descriptor tag). */
    file_type = fe[16 + 11];
    icb_flags = rd_le16(fe + 16 + 18);
    alloc_type = icb_flags & 0x0007;

    if (tag_id == UDF_TAG_FILE_ENTRY) {
        info_length_off = 56;
        l_ea = rd_le32(fe + 168);
        l_ad = rd_le32(fe + 172);
        base_offset = 176u + l_ea;
    } else {
        /* EFE: fixed header is 216 bytes.
         * L_EA at 208, L_AD at 212, EAs start at 216. */
        info_length_off = 56;
        l_ea = rd_le32(fe + 208);
        l_ad = rd_le32(fe + 212);
        base_offset = 216u + l_ea;
    }
    (void)base_offset;

    memset(out, 0, sizeof(*out));
    out->fs = fs;
    out->icb_block = abs_lbn;
    out->size = rd_le64(fe + info_length_off);
    out->is_dir = (file_type == UDF_FILE_TYPE_DIR);
    out->data.size = out->size;

    /* Reuse alloc-desc parser with the right base by rewriting fe layout in-place:
     * The parser hardcodes 176 + L_EA — for EFE we shift the data so that
     * the parser sees ADs at that offset. */
    if (tag_id == UDF_TAG_EXT_FE) {
        /* Move ADs so parser's base of 176+L_EA matches. */
        memmove(fe + 176u + l_ea, fe + 216u + l_ea, l_ad);
    }
    if (udf_parse_alloc_descs(fs, fe, UDF_LB_SIZE, alloc_type, l_ea, l_ad, &out->data) < 0)
        return -EIO;
    return 0;
}

/* ---------------- Directory iteration (FIDs) ---------------- */

typedef struct {
    char     name[256];
    uint32_t icb_block;    /* absolute LBN */
    int      is_dir;
    int      deleted;
    int      parent;
} udf_dir_entry_t;

typedef int (*udf_dir_cb)(void *ctx, const udf_dir_entry_t *e);

static void udf_decode_dstring(const uint8_t *raw, uint8_t len, char *out, int max_out)
{
    int j = 0;
    if (len == 0) {
        out[0] = 0;
        return;
    }
    if (raw[0] == 8) {
        int i;
        for (i = 1; i < len && j < max_out - 1; i++)
            out[j++] = (char)raw[i];
    } else if (raw[0] == 16) {
        int i;
        for (i = 1; i + 1 < len && j < max_out - 1; i += 2) {
            uint16_t c = ((uint16_t)raw[i] << 8) | raw[i + 1];  /* UTF-16BE */
            out[j++] = (c < 0x80) ? (char)c : '?';
        }
    } else {
        int i;
        for (i = 0; i < len && j < max_out - 1; i++)
            out[j++] = (char)raw[i];
    }
    out[j] = 0;
}

static int udf_iterate_dir(udf_node_t *dir, udf_dir_cb cb, void *ctx)
{
    uint64_t off = 0;
    uint8_t *buf;

    if (!dir->is_dir)
        return -ENOTDIR;

    buf = (uint8_t *)kmalloc((size_t)dir->size);
    if (!buf)
        return -ENOMEM;
    if (udf_read_content(dir->fs, &dir->data, 0, (uint32_t)dir->size, buf) < 0)
        return -EIO;

    while (off + 38 <= dir->size) {
        uint16_t tag_id;
        uint8_t  fchar;
        uint8_t  l_fi;
        uint16_t l_iu;
        udf_long_ad_t icb;
        udf_dir_entry_t de;
        uint32_t rec_len;

        tag_id = rd_le16(buf + off);
        if (tag_id != UDF_TAG_FID)
            break;
        fchar = buf[off + 18];
        l_fi  = buf[off + 19];
        udf_parse_long_ad(buf + off + 20, &icb);
        l_iu  = rd_le16(buf + off + 36);
        rec_len = 38u + l_iu + l_fi;
        rec_len = (rec_len + 3u) & ~3u;
        if (off + rec_len > dir->size)
            break;

        memset(&de, 0, sizeof(de));
        de.deleted = (fchar & UDF_FID_DELETED) != 0;
        de.is_dir  = (fchar & UDF_FID_DIRECTORY) != 0;
        de.parent  = (fchar & UDF_FID_PARENT) != 0;
        de.icb_block = udf_resolve_lbn(dir->fs, icb.location.block, icb.location.partition);
        udf_decode_dstring(buf + off + 38 + l_iu, l_fi, de.name, sizeof(de.name));

        if (!de.deleted && !de.parent && de.name[0]) {
            int rc = cb(ctx, &de);
            if (rc)
                return rc;
        }
        off += rec_len;
    }
    return 0;
}

typedef struct {
    const char *want;
    udf_dir_entry_t match;
    int found;
} udf_lookup_ctx_t;

static int udf_cieq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int udf_lookup_cb(void *ctx, const udf_dir_entry_t *e)
{
    udf_lookup_ctx_t *c = (udf_lookup_ctx_t *)ctx;
    if (udf_cieq(e->name, c->want)) {
        c->match = *e;
        c->found = 1;
        return 1;
    }
    return 0;
}

typedef struct {
    vfs_dirent_t *out;
    size_t max;
    size_t written;
} udf_readdir_ctx_t;

static int udf_readdir_cb(void *ctx, const udf_dir_entry_t *e)
{
    udf_readdir_ctx_t *c = (udf_readdir_ctx_t *)ctx;
    if (c->written >= c->max)
        return 1;
    strncpy(c->out[c->written].name, e->name, sizeof(c->out[c->written].name) - 1);
    c->out[c->written].name[sizeof(c->out[c->written].name) - 1] = 0;
    c->out[c->written].type = e->is_dir ? S_IFDIR : S_IFREG;
    c->out[c->written].ino  = e->icb_block;
    c->written++;
    return 0;
}

/* ---------------- VFS ops ---------------- */

static ssize_t udf_f_read(file_t *file, void *buf, size_t count, off_t *pos)
{
    udf_node_t *n = (udf_node_t *)file->f_inode->i_private;
    int rc;
    if (!n) return -EIO;
    if (n->is_dir) return -EISDIR;
    if ((uint64_t)*pos >= n->size)
        return 0;
    if ((uint64_t)*pos + count > n->size)
        count = (size_t)(n->size - (uint64_t)*pos);
    rc = udf_read_content(n->fs, &n->data, (uint64_t)*pos, (uint32_t)count, (uint8_t *)buf);
    if (rc < 0) return rc;
    *pos += rc;
    return rc;
}

static int udf_f_readdir(file_t *file, void *dirent, size_t max)
{
    udf_node_t *n = (udf_node_t *)file->f_inode->i_private;
    udf_readdir_ctx_t ctx;
    int rc;
    if (!n || !n->is_dir)
        return -ENOTDIR;
    ctx.out = (vfs_dirent_t *)dirent;
    ctx.max = max;
    ctx.written = 0;
    rc = udf_iterate_dir(n, udf_readdir_cb, &ctx);
    if (rc < 0) return rc;
    return (int)ctx.written;
}

static dentry_t *udf_lookup(inode_t *dir, dentry_t *dentry)
{
    udf_node_t *dn = (udf_node_t *)dir->i_private;
    udf_lookup_ctx_t lc;
    udf_node_t *child;
    inode_t *ino;
    dentry_t *out;

    if (!dn || !dn->is_dir)
        return NULL;
    memset(&lc, 0, sizeof(lc));
    lc.want = dentry->d_name;
    if (udf_iterate_dir(dn, udf_lookup_cb, &lc) < 0)
        return NULL;
    if (!lc.found)
        return NULL;

    child = (udf_node_t *)kmalloc(sizeof(*child));
    if (!child)
        return NULL;
    if (udf_load_node(dn->fs, lc.match.icb_block, child) < 0)
        return NULL;

    ino = alloc_inode(dir->i_sb, child->is_dir ? (S_IFDIR | 0555) : (S_IFREG | 0444));
    if (!ino) return NULL;
    ino->i_size = child->size;
    ino->i_ino  = child->icb_block;
    ino->i_private = child;
    ino->i_op  = &udf_iops;
    ino->i_fop = &udf_fops;

    out = alloc_dentry(dentry->d_name, dentry->d_parent, ino);
    return out;
}

static const inode_operations_t udf_iops = {
    .lookup = udf_lookup,
};

static const file_operations_t udf_fops = {
    .read    = udf_f_read,
    .readdir = udf_f_readdir,
};

/* ---------------- Volume descriptor scanning ---------------- */

static int udf_parse_vds(udf_fs_t *fs, uint32_t start_lbn, uint32_t bytes,
                         uint32_t *out_part_start, uint32_t *out_part_len,
                         udf_long_ad_t *out_fsd)
{
    uint8_t *lb = (uint8_t *)kmalloc(UDF_LB_SIZE);
    uint32_t count = bytes / UDF_LB_SIZE;
    uint32_t i;

    if (!lb) return -ENOMEM;
    for (i = 0; i < count; i++) {
        uint32_t abs = start_lbn + i;  /* volumes usually start at LBN 0 */
        uint16_t tag_id;
        if (udf_read_lb(fs, abs, lb) < 0)
            continue;
        tag_id = rd_le16(lb + 0);
        if (tag_id == UDF_TAG_TERM)
            break;
        if (tag_id == UDF_TAG_PART_DESC) {
            /* Partition Descriptor:
             *   at offset 188: PartitionStartingLocation (4)
             *   at offset 192: PartitionLength (4)
             */
            *out_part_start = rd_le32(lb + 188);
            *out_part_len   = rd_le32(lb + 192);
        } else if (tag_id == UDF_TAG_LVD) {
            /* Logical Volume Descriptor:
             *   FileSetDescriptorLocation (long_ad) at offset 248
             *   (16 bytes VolumeDescSeqNum + 32 DescCharSet + 128 LogVolIdent +
             *    4 LogicalBlockSize + 32 DomainIdent = 16 + 32 + 128 + 4 + 32 = 212
             *    from tag end (16). 16 + 212 = 228. Then + 16 for LogVolContentUse
             *    which contains the fileset long_ad.)
             * Actually per spec: LogVolContentUse (16 bytes) at offset 248.
             * We treat it as a long_ad.
             */
            udf_parse_long_ad(lb + 248, out_fsd);
        }
    }
    return 0;
}

/* ---------------- Mount ---------------- */

static int udf_mount(file_system_type_t *fs_type, int flags,
                     const char *dev_name, void *data, super_block_t **sb_out)
{
    const block_api_t *blk = block_api_get();
    block_device_t *bdev;
    uint8_t *lb;
    udf_fs_t *fs;
    udf_node_t *root;
    inode_t *rino;
    dentry_t *rd;
    super_block_t *sb;
    uint32_t vds_len, vds_loc;
    uint32_t part_start = 0, part_len = 0;
    udf_long_ad_t fsd_ad;
    udf_long_ad_t root_ad;

    (void)flags; (void)data;
    if (!blk || !dev_name)
        return -EINVAL;
    bdev = blk->lookup(dev_name);
    if (!bdev)
        return -ENODEV;

    lb = (uint8_t *)kmalloc(UDF_LB_SIZE);
    if (!lb)
        return -ENOMEM;

    /* Read anchor. */
    if (blk->read(bdev, (uint64_t)UDF_ANCHOR_LBN * UDF_LB_TO_SECT, lb, UDF_LB_TO_SECT) < 0)
        return -EIO;
    if (rd_le16(lb + 0) != UDF_TAG_AVDP)
        return -EINVAL;
    vds_len = rd_le32(lb + 16);
    vds_loc = rd_le32(lb + 20);

    fs = (udf_fs_t *)kmalloc(sizeof(*fs));
    if (!fs)
        return -ENOMEM;
    memset(fs, 0, sizeof(*fs));
    fs->bdev = bdev;

    memset(&fsd_ad, 0, sizeof(fsd_ad));
    if (udf_parse_vds(fs, vds_loc, vds_len, &part_start, &part_len, &fsd_ad) < 0)
        return -EIO;
    if (part_start == 0 && part_len == 0)
        return -EINVAL;
    fs->partition_start = part_start;
    fs->partition_length = part_len;

    /* Read File Set Descriptor. */
    {
        uint32_t fsd_abs = udf_resolve_lbn(fs, fsd_ad.location.block, fsd_ad.location.partition);
        if (blk->read(bdev, (uint64_t)fsd_abs * UDF_LB_TO_SECT, lb, UDF_LB_TO_SECT) < 0)
            return -EIO;
        if (rd_le16(lb + 0) != UDF_TAG_FSD)
            return -EINVAL;
        /*
         * File Set Descriptor:
         *   tag (16) + RecordingDateTime (12) + InterchangeLevel (2) +
         *   MaxInterchangeLevel (2) + CharSetList (4) + MaxCharSetList (4)
         *   + FileSetNumber (4) + FileSetDescriptorNumber (4)
         *   + LogVolIdentCharSet (64) + LogVolIdent (128)
         *   + FileSetCharSet (64) + FileSetIdent (32) + CopyrightFileIdent (32)
         *   + AbstractFileIdent (32) + RootDirectoryICB (long_ad 16)
         *   = 16 + 12 + 2 + 2 + 4 + 4 + 4 + 4 + 64 + 128 + 64 + 32 + 32 + 32 = 400
         *   RootDirectoryICB at offset 400.
         */
        udf_parse_long_ad(lb + 400, &root_ad);
    }
    fs->root_icb_block     = root_ad.location.block;
    fs->root_icb_partition = root_ad.location.partition;
    fs->root_icb_length    = root_ad.length_flags & 0x3FFFFFFFu;

    root = (udf_node_t *)kmalloc(sizeof(*root));
    sb   = (super_block_t *)kmalloc(sizeof(*sb));
    if (!root || !sb)
        return -ENOMEM;
    {
        uint32_t root_abs = udf_resolve_lbn(fs, fs->root_icb_block, fs->root_icb_partition);
        if (udf_load_node(fs, root_abs, root) < 0)
            return -EIO;
    }
    if (!root->is_dir)
        return -EINVAL;

    memset(sb, 0, sizeof(*sb));
    rino = alloc_inode(sb, S_IFDIR | 0555);
    if (!rino) return -ENOMEM;
    rino->i_size = root->size;
    rino->i_ino  = root->icb_block;
    rino->i_private = root;
    rino->i_op  = &udf_iops;
    rino->i_fop = &udf_fops;

    rd = alloc_dentry("/", NULL, rino);
    if (!rd) return -ENOMEM;

    sb->s_type = fs_type;
    sb->s_root = rd;
    sb->s_bdev = bdev;
    sb->s_fs_info = fs;
    *sb_out = sb;

    vga_print("udf: mounted ");
    vga_print(dev_name);
    vga_print(" (part=");
    vga_print_uint(part_start);
    vga_print(")\n");
    return 0;
}

/* ---------------- Registration ---------------- */

static file_system_type_t g_fs = {
    .name  = "udf",
    .mount = udf_mount,
};

static int udf_init(driver_t *drv, void *ctx)
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
    vga_print("udf: registered\n");
    return 0;
}

int kmod_init(void)
{
    driver_t d;
    memset(&d, 0, sizeof(d));
    strncpy(d.name,    "udf", DRIVER_NAME_MAX    - 1);
    strncpy(d.version, "1.0", DRIVER_VERSION_MAX - 1);
    d.kind     = DRIVER_KIND_CUSTOM;
    d.class    = DRIVER_CLASS_FS;
    d.priority = 65;
    d.init     = udf_init;
    if (driver_register(&d) < 0)
        return -1;
    if (driver_load("udf", NULL) < 0)
        return -1;
    return 0;
}
