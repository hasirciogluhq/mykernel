/*
 * NTFS read-only filesystem driver.
 *
 * Parses the boot sector, bootstraps the $MFT by decoding its own data
 * runs from MFT record 0, then walks arbitrary MFT records applying the
 * standard update-sequence fixup.  For each record it iterates the
 * attribute list looking for $FILE_NAME (0x30), $DATA (0x80),
 * $INDEX_ROOT (0x90) and $INDEX_ALLOCATION (0xA0), handling both
 * resident and non-resident forms with an explicit runlist decoder.
 *
 * Directory listing:
 *   - $INDEX_ROOT provides the root of the B+-tree.  Small directories
 *     are stored entirely in $INDEX_ROOT; larger ones use $INDEX_ALLOCATION
 *     to hold 4KiB INDX buffers referenced by VCN.  This driver walks the
 *     B+ tree by descending sub-nodes when present.
 *
 * File data:
 *   - Resident $DATA is copied straight out of the MFT record.
 *   - Non-resident $DATA is served by walking the DATA runlist and
 *     reading each run's clusters directly from the block device.
 *
 * NTFS is little-endian on disk.  We only support UTF-16LE filename
 * comparison by folding to ASCII for characters below 0x80 — enough for
 * typical POSIX filenames.
 */

#include <kernel/vfs_api.h>
#include <kernel/block_api.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/string.h>
#include <drivers/vfs_fs.h>
#include <drivers/driver.h>
#include <drivers/vga.h>

/* ---------------- Constants ---------------- */

#define NTFS_ROOT_MFT           5u

#define NTFS_ATTR_STDINFO       0x10u
#define NTFS_ATTR_ATTR_LIST     0x20u
#define NTFS_ATTR_FILE_NAME     0x30u
#define NTFS_ATTR_DATA          0x80u
#define NTFS_ATTR_INDEX_ROOT    0x90u
#define NTFS_ATTR_INDEX_ALLOC   0xA0u
#define NTFS_ATTR_BITMAP        0xB0u
#define NTFS_ATTR_END           0xFFFFFFFFu

#define NTFS_FILE_FLAG_INUSE    0x0001u
#define NTFS_FILE_FLAG_DIR      0x0002u

#define NTFS_INDEX_HAS_SUBNODES 0x01u
#define NTFS_INDEX_LAST_ENTRY   0x02u

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

/* ---------------- Types ---------------- */

typedef struct {
    uint64_t vcn;    /* starting VCN of this run */
    int64_t  lcn;    /* physical LCN, or 0 if sparse */
    uint64_t len;    /* clusters */
    int      sparse;
} ntfs_run_t;

typedef struct {
    ntfs_run_t *runs;
    int         nruns;
    uint64_t    data_size;
    uint64_t    total_clusters;
    int         resident;
    uint8_t    *resident_data;
    uint32_t    resident_size;
} ntfs_runlist_t;

typedef struct {
    block_device_t *bdev;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t cluster_size;
    uint32_t mft_record_size;
    uint32_t index_record_size;
    uint64_t mft_lcn;
    ntfs_runlist_t mft_runlist;
} ntfs_fs_t;

typedef struct {
    ntfs_fs_t *fs;
    uint64_t   mft_no;
    uint64_t   size;
    int        is_dir;
    ntfs_runlist_t data;
    ntfs_runlist_t index_root_data;   /* resident */
    ntfs_runlist_t index_alloc_data;  /* non-resident allocation */
    int              has_index;
} ntfs_node_t;

static dentry_t *(*alloc_dentry)(const char *, dentry_t *, inode_t *);
static inode_t  *(*alloc_inode)(super_block_t *, uint32_t);

static const inode_operations_t ntfs_iops;
static const file_operations_t  ntfs_fops;

/* ---------------- Block I/O ---------------- */

static int ntfs_read_sectors(ntfs_fs_t *fs, uint64_t sec, uint32_t count, void *buf)
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

static int ntfs_read_cluster_range(ntfs_fs_t *fs, uint64_t lcn, uint32_t count, void *buf)
{
    return ntfs_read_sectors(fs, lcn * fs->sectors_per_cluster,
                             count * fs->sectors_per_cluster, buf);
}

/* ---------------- Update-sequence fixup ---------------- */

static int ntfs_apply_fixup(uint8_t *record, uint32_t record_size, uint32_t bytes_per_sector)
{
    uint16_t usa_offset = rd_le16(record + 4);
    uint16_t usa_count  = rd_le16(record + 6);
    uint16_t usn;
    uint32_t i;

    if (usa_offset + usa_count * 2u > record_size)
        return -EINVAL;
    if (usa_count == 0)
        return -EINVAL;

    usn = rd_le16(record + usa_offset);
    for (i = 1; i < usa_count; i++) {
        uint32_t sector_end = i * bytes_per_sector - 2;
        if (sector_end + 2 > record_size)
            return -EINVAL;
        if (rd_le16(record + sector_end) != usn)
            return -EINVAL;
        record[sector_end + 0] = record[usa_offset + i * 2 + 0];
        record[sector_end + 1] = record[usa_offset + i * 2 + 1];
    }
    return 0;
}

/* ---------------- Data-run decoding ---------------- */

static int ntfs_decode_runlist(const uint8_t *runs, uint32_t max_bytes,
                               uint64_t data_size, ntfs_runlist_t *out)
{
    int64_t  prev_lcn = 0;
    uint64_t vcn = 0;
    uint32_t off = 0;
    int cap = 8;
    int cnt = 0;

    memset(out, 0, sizeof(*out));
    out->runs = (ntfs_run_t *)kmalloc(sizeof(ntfs_run_t) * cap);
    if (!out->runs)
        return -ENOMEM;

    while (off < max_bytes) {
        uint8_t header = runs[off++];
        uint8_t len_size  = header & 0x0F;
        uint8_t off_size  = (header >> 4) & 0x0F;
        uint64_t len_val = 0;
        int64_t  off_val = 0;
        int i;

        if (header == 0)
            break;
        if (off + len_size + off_size > max_bytes)
            break;

        for (i = 0; i < len_size; i++)
            len_val |= ((uint64_t)runs[off + i]) << (i * 8);
        off += len_size;

        if (off_size == 0) {
            off_val = 0; /* sparse run */
        } else {
            uint64_t raw = 0;
            for (i = 0; i < off_size; i++)
                raw |= ((uint64_t)runs[off + i]) << (i * 8);
            /* sign-extend from off_size bytes */
            if (runs[off + off_size - 1] & 0x80) {
                if (off_size < 8) {
                    uint64_t mask = ~((((uint64_t)1) << (off_size * 8)) - 1);
                    raw |= mask;
                }
            }
            off_val = (int64_t)raw;
            off += off_size;
        }

        if (cnt >= cap) {
            int newcap = cap * 2;
            ntfs_run_t *nr = (ntfs_run_t *)kmalloc(sizeof(ntfs_run_t) * newcap);
            if (!nr)
                return -ENOMEM;
            memcpy(nr, out->runs, sizeof(ntfs_run_t) * cap);
            out->runs = nr;
            cap = newcap;
        }
        out->runs[cnt].vcn = vcn;
        out->runs[cnt].len = len_val;
        if (off_size == 0) {
            out->runs[cnt].lcn = 0;
            out->runs[cnt].sparse = 1;
        } else {
            prev_lcn += off_val;
            out->runs[cnt].lcn = prev_lcn;
            out->runs[cnt].sparse = 0;
        }
        vcn += len_val;
        cnt++;
    }
    out->nruns = cnt;
    out->total_clusters = vcn;
    out->data_size = data_size;
    return 0;
}

static int ntfs_read_runlist(ntfs_fs_t *fs, const ntfs_runlist_t *rl,
                             uint64_t offset, uint32_t bytes, uint8_t *out)
{
    uint32_t done = 0;
    uint8_t *cbuf;

    if (rl->resident) {
        if (offset >= rl->resident_size)
            return 0;
        if (offset + bytes > rl->resident_size)
            bytes = rl->resident_size - (uint32_t)offset;
        memcpy(out, rl->resident_data + offset, bytes);
        return (int)bytes;
    }

    if (offset >= rl->data_size)
        return 0;
    if (offset + bytes > rl->data_size)
        bytes = (uint32_t)(rl->data_size - offset);

    cbuf = (uint8_t *)kmalloc(fs->cluster_size);
    if (!cbuf)
        return -ENOMEM;

    while (done < bytes) {
        uint64_t abs_off  = offset + done;
        uint64_t vcn      = abs_off / fs->cluster_size;
        uint32_t in_off   = (uint32_t)(abs_off % fs->cluster_size);
        int i;
        int found = 0;
        for (i = 0; i < rl->nruns; i++) {
            const ntfs_run_t *r = &rl->runs[i];
            if (vcn >= r->vcn && vcn < r->vcn + r->len) {
                uint32_t chunk = fs->cluster_size - in_off;
                if (chunk > bytes - done)
                    chunk = bytes - done;
                if (r->sparse) {
                    memset(out + done, 0, chunk);
                } else {
                    uint64_t lcn = (uint64_t)r->lcn + (vcn - r->vcn);
                    if (ntfs_read_cluster_range(fs, lcn, 1, cbuf) < 0)
                        return -EIO;
                    memcpy(out + done, cbuf + in_off, chunk);
                }
                done += chunk;
                found = 1;
                break;
            }
        }
        if (!found)
            break;
    }
    return (int)done;
}

/* ---------------- Attribute walking ---------------- */

typedef struct {
    uint32_t type;
    uint32_t length;
    uint8_t  non_resident;
    uint32_t attr_offset;   /* offset within the record */

    /* resident */
    uint32_t val_length;
    uint16_t val_offset;

    /* non-resident */
    uint64_t data_size;
    uint16_t runs_offset;
} ntfs_attr_t;

static int ntfs_first_attr(const uint8_t *record, uint32_t record_size, ntfs_attr_t *a)
{
    a->attr_offset = rd_le16(record + 20);
    (void)record_size;
    return 0;
}

static int ntfs_next_attr(const uint8_t *record, uint32_t record_size, ntfs_attr_t *a)
{
    uint32_t off = a->attr_offset;
    uint32_t type;
    uint32_t len;

    if (off + 4 > record_size)
        return -1;
    type = rd_le32(record + off);
    if (type == NTFS_ATTR_END)
        return -1;
    if (off + 16 > record_size)
        return -1;
    len = rd_le32(record + off + 4);
    if (len == 0 || off + len > record_size)
        return -1;

    a->type = type;
    a->length = len;
    a->non_resident = record[off + 8];

    if (!a->non_resident) {
        a->val_length = rd_le32(record + off + 16);
        a->val_offset = rd_le16(record + off + 20);
        a->data_size = a->val_length;
    } else {
        a->data_size  = rd_le64(record + off + 48);
        a->runs_offset = rd_le16(record + off + 32);
    }
    return 0;
}

static void ntfs_advance_attr(ntfs_attr_t *a)
{
    a->attr_offset += a->length;
}

/* ---------------- Read MFT record ---------------- */

static int ntfs_read_mft_record(ntfs_fs_t *fs, uint64_t mft_no, uint8_t *out)
{
    uint32_t rs = fs->mft_record_size;
    uint64_t byte_off = mft_no * rs;

    if (fs->mft_runlist.runs == NULL) {
        /* Bootstrap read: use raw MFT_LCN. */
        uint64_t sector = fs->mft_lcn * fs->sectors_per_cluster +
                          (byte_off / fs->bytes_per_sector);
        uint32_t count = (rs + fs->bytes_per_sector - 1) / fs->bytes_per_sector;
        if (ntfs_read_sectors(fs, sector, count, out) < 0)
            return -EIO;
    } else {
        int rc = ntfs_read_runlist(fs, &fs->mft_runlist, byte_off, rs, out);
        if (rc < 0)
            return rc;
        if ((uint32_t)rc < rs)
            return -EIO;
    }
    if (memcmp(out, "FILE", 4) != 0)
        return -EINVAL;
    if (ntfs_apply_fixup(out, rs, fs->bytes_per_sector) < 0)
        return -EINVAL;
    return 0;
}

/* ---------------- Parse an attribute → runlist ---------------- */

static int ntfs_parse_attr_data(ntfs_fs_t *fs,
                                const uint8_t *record,
                                const ntfs_attr_t *a,
                                ntfs_runlist_t *out)
{
    (void)fs;
    memset(out, 0, sizeof(*out));
    if (!a->non_resident) {
        out->resident = 1;
        out->resident_data = (uint8_t *)kmalloc(a->val_length);
        if (!out->resident_data)
            return -ENOMEM;
        memcpy(out->resident_data, record + a->attr_offset + a->val_offset, a->val_length);
        out->resident_size = a->val_length;
        out->data_size = a->val_length;
        return 0;
    }
    return ntfs_decode_runlist(record + a->attr_offset + a->runs_offset,
                               a->length - a->runs_offset,
                               a->data_size, out);
}

/* ---------------- Filename UTF-16LE handling ---------------- */

static void ntfs_utf16_to_ascii(const uint8_t *u16, int chars, char *out, int max)
{
    int i, j = 0;
    for (i = 0; i < chars && j < max - 1; i++) {
        uint16_t cp = rd_le16(u16 + i * 2);
        if (cp < 0x80)
            out[j++] = (char)cp;
        else
            out[j++] = '?';
    }
    out[j] = 0;
}

static int ntfs_cieq(const char *a, const char *b)
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

/* ---------------- Index entry iteration ---------------- */

typedef struct {
    char     name[256];
    uint64_t mft_ref;
    int      is_dir;
    uint64_t size;
} ntfs_dir_entry_t;

typedef int (*ntfs_dir_cb)(void *ctx, const ntfs_dir_entry_t *e);

static int ntfs_iter_entries(ntfs_fs_t *fs,
                             const uint8_t *entries, uint32_t max,
                             ntfs_dir_cb cb, void *ctx,
                             ntfs_runlist_t *alloc_rl, int allow_recurse)
{
    uint32_t off = 0;
    (void)fs;
    while (off + 16 <= max) {
        uint64_t mft_ref = rd_le64(entries + off + 0);
        uint16_t ent_len = rd_le16(entries + off + 8);
        uint16_t stream_len = rd_le16(entries + off + 10);
        uint32_t flags = rd_le32(entries + off + 12);
        int last = (flags & NTFS_INDEX_LAST_ENTRY) != 0;
        int has_sub = (flags & NTFS_INDEX_HAS_SUBNODES) != 0;

        if (ent_len == 0 || off + ent_len > max)
            break;

        if (!last && stream_len >= 66) {
            /* FILE_NAME record starts at entries + off + 16. */
            const uint8_t *fn = entries + off + 16;
            uint8_t name_len = fn[64];
            uint8_t namespace_id = fn[65];
            ntfs_dir_entry_t de;
            uint32_t flags_fn;

            (void)namespace_id;
            memset(&de, 0, sizeof(de));
            de.mft_ref = mft_ref & 0x0000FFFFFFFFFFFFULL;
            de.size    = rd_le64(fn + 48);
            flags_fn   = rd_le32(fn + 56);
            de.is_dir  = (flags_fn & 0x10000000u) != 0; /* NTFS_FILE_ATTR_I30_INDEX_PRESENT */
            ntfs_utf16_to_ascii(fn + 66, name_len, de.name, sizeof(de.name));

            /* Filter DOS-only namespace (2) — those are 8.3 aliases and
             * always have a corresponding WIN32/POSIX entry. */
            if (namespace_id != 2) {
                int rc = cb(ctx, &de);
                if (rc)
                    return rc;
            }
        }

        if (has_sub && alloc_rl && allow_recurse) {
            /* VCN of subnode: 8 bytes at end of this entry, aligned. */
            uint64_t sub_vcn = rd_le64(entries + off + ent_len - 8);
            uint8_t *idx_buf = (uint8_t *)kmalloc(fs->index_record_size);
            int rc;
            if (!idx_buf)
                return -ENOMEM;
            rc = ntfs_read_runlist(fs, alloc_rl,
                                   sub_vcn * fs->cluster_size,
                                   fs->index_record_size, idx_buf);
            if (rc == (int)fs->index_record_size &&
                memcmp(idx_buf, "INDX", 4) == 0 &&
                ntfs_apply_fixup(idx_buf, fs->index_record_size, fs->bytes_per_sector) == 0) {
                /*
                 * After the standard INDX record header comes the
                 * INDEX_HEADER (16 bytes) at offset 24 (0x18). The entry
                 * area then starts at (0x18 + first_entry_offset).
                 */
                uint32_t ih = 24;
                uint32_t first = rd_le32(idx_buf + ih + 0);
                uint32_t total = rd_le32(idx_buf + ih + 4);
                int rc2 = ntfs_iter_entries(fs, idx_buf + ih + first,
                                            total - first, cb, ctx,
                                            alloc_rl, allow_recurse);
                if (rc2)
                    return rc2;
            }
        }

        if (last)
            break;
        off += ent_len;
    }
    return 0;
}

static int ntfs_iterate_directory(ntfs_node_t *n, ntfs_dir_cb cb, void *ctx)
{
    ntfs_fs_t *fs = n->fs;
    const uint8_t *root_data;
    uint32_t root_size;
    uint32_t first;
    uint32_t total;

    if (!n->has_index || !n->index_root_data.resident_data)
        return -ENOTDIR;

    root_data = n->index_root_data.resident_data;
    root_size = n->index_root_data.resident_size;

    /*
     * $INDEX_ROOT layout (attribute value):
     *   0..3   Attribute type (0x30)
     *   4..7   Collation rule
     *   8..11  Bytes per index record
     *   12     Clusters per index record
     *   13..15 padding
     *   16..31 INDEX_HEADER:
     *          16..19 First entry offset (rel to INDEX_HEADER)
     *          20..23 Total size of entries
     *          24..27 Allocated size of entries
     *          28     Flags
     */
    if (root_size < 32)
        return -EINVAL;
    first = rd_le32(root_data + 16);
    total = rd_le32(root_data + 20);
    if (16 + first > root_size)
        return -EINVAL;
    if (16 + total > root_size)
        total = root_size - 16;

    return ntfs_iter_entries(fs, root_data + 16 + first,
                             total - first, cb, ctx,
                             n->has_index ? &n->index_alloc_data : NULL,
                             1);
}

/* ---------------- Building an ntfs_node from an MFT record ---------------- */

static int ntfs_build_node(ntfs_fs_t *fs, uint64_t mft_no, ntfs_node_t *out)
{
    uint8_t *rec = (uint8_t *)kmalloc(fs->mft_record_size);
    ntfs_attr_t a;
    if (!rec) return -ENOMEM;
    if (ntfs_read_mft_record(fs, mft_no, rec) < 0)
        return -EIO;

    memset(out, 0, sizeof(*out));
    out->fs = fs;
    out->mft_no = mft_no;
    out->is_dir = (rd_le16(rec + 22) & NTFS_FILE_FLAG_DIR) != 0;

    ntfs_first_attr(rec, fs->mft_record_size, &a);
    while (ntfs_next_attr(rec, fs->mft_record_size, &a) == 0) {
        if (a.type == NTFS_ATTR_DATA && a.non_resident == 0 && a.val_length > 0) {
            /* Only take the unnamed $DATA; skip named ADS. */
            uint8_t nlen = rec[a.attr_offset + 9];
            if (nlen == 0) {
                ntfs_parse_attr_data(fs, rec, &a, &out->data);
                out->size = a.val_length;
            }
        } else if (a.type == NTFS_ATTR_DATA && a.non_resident) {
            uint8_t nlen = rec[a.attr_offset + 9];
            if (nlen == 0) {
                ntfs_parse_attr_data(fs, rec, &a, &out->data);
                out->size = a.data_size;
            }
        } else if (a.type == NTFS_ATTR_INDEX_ROOT) {
            ntfs_parse_attr_data(fs, rec, &a, &out->index_root_data);
            out->has_index = 1;
        } else if (a.type == NTFS_ATTR_INDEX_ALLOC) {
            ntfs_parse_attr_data(fs, rec, &a, &out->index_alloc_data);
        } else if (a.type == NTFS_ATTR_FILE_NAME && a.non_resident == 0) {
            /* Take file size from the primary FILE_NAME (namespace != DOS). */
            uint8_t ns = rec[a.attr_offset + a.val_offset + 65];
            if (ns != 2 && out->size == 0) {
                uint64_t real = rd_le64(rec + a.attr_offset + a.val_offset + 48);
                out->size = real;
            }
        }
        ntfs_advance_attr(&a);
    }
    return 0;
}

/* ---------------- Lookup callback ---------------- */

typedef struct {
    const char *want;
    ntfs_dir_entry_t match;
    int found;
} ntfs_lookup_ctx_t;

static int ntfs_lookup_cb(void *ctx, const ntfs_dir_entry_t *e)
{
    ntfs_lookup_ctx_t *c = (ntfs_lookup_ctx_t *)ctx;
    if (ntfs_cieq(e->name, c->want)) {
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
} ntfs_readdir_ctx_t;

static int ntfs_readdir_cb(void *ctx, const ntfs_dir_entry_t *e)
{
    ntfs_readdir_ctx_t *c = (ntfs_readdir_ctx_t *)ctx;
    if (c->written >= c->max)
        return 1;
    if (e->name[0] == '$') /* skip metafiles like $MFT, $LogFile, etc. */
        return 0;
    strncpy(c->out[c->written].name, e->name, sizeof(c->out[c->written].name) - 1);
    c->out[c->written].name[sizeof(c->out[c->written].name) - 1] = 0;
    c->out[c->written].type = e->is_dir ? S_IFDIR : S_IFREG;
    c->out[c->written].ino  = (uint32_t)e->mft_ref;
    c->written++;
    return 0;
}

/* ---------------- VFS ops ---------------- */

static ssize_t ntfs_f_read(file_t *file, void *buf, size_t count, off_t *pos)
{
    ntfs_node_t *n = (ntfs_node_t *)file->f_inode->i_private;
    int rc;
    if (!n) return -EIO;
    if (n->is_dir) return -EISDIR;
    if ((uint64_t)*pos >= n->size)
        return 0;
    if ((uint64_t)*pos + count > n->size)
        count = (size_t)(n->size - (uint64_t)*pos);
    rc = ntfs_read_runlist(n->fs, &n->data, (uint64_t)*pos, (uint32_t)count, (uint8_t *)buf);
    if (rc < 0) return rc;
    *pos += rc;
    return rc;
}

static int ntfs_f_readdir(file_t *file, void *dirent, size_t max)
{
    ntfs_node_t *n = (ntfs_node_t *)file->f_inode->i_private;
    ntfs_readdir_ctx_t ctx;
    int rc;
    if (!n || !n->is_dir)
        return -ENOTDIR;
    ctx.out = (vfs_dirent_t *)dirent;
    ctx.max = max;
    ctx.written = 0;
    rc = ntfs_iterate_directory(n, ntfs_readdir_cb, &ctx);
    if (rc < 0) return rc;
    return (int)ctx.written;
}

static dentry_t *ntfs_lookup(inode_t *dir, dentry_t *dentry)
{
    ntfs_node_t *dn = (ntfs_node_t *)dir->i_private;
    ntfs_lookup_ctx_t lc;
    ntfs_node_t *child;
    inode_t *ino;
    dentry_t *out;

    if (!dn || !dn->is_dir)
        return NULL;
    memset(&lc, 0, sizeof(lc));
    lc.want = dentry->d_name;
    if (ntfs_iterate_directory(dn, ntfs_lookup_cb, &lc) < 0)
        return NULL;
    if (!lc.found)
        return NULL;

    child = (ntfs_node_t *)kmalloc(sizeof(*child));
    if (!child)
        return NULL;
    memset(child, 0, sizeof(*child));
    if (ntfs_build_node(dn->fs, lc.match.mft_ref, child) < 0)
        return NULL;

    ino = alloc_inode(dir->i_sb, child->is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644));
    if (!ino) return NULL;
    ino->i_size = child->size;
    ino->i_ino  = (uint32_t)lc.match.mft_ref;
    ino->i_private = child;
    ino->i_op  = &ntfs_iops;
    ino->i_fop = &ntfs_fops;

    out = alloc_dentry(dentry->d_name, dentry->d_parent, ino);
    return out;
}

static const inode_operations_t ntfs_iops = {
    .lookup = ntfs_lookup,
};

static const file_operations_t ntfs_fops = {
    .read    = ntfs_f_read,
    .readdir = ntfs_f_readdir,
};

/* ---------------- Mount ---------------- */

static int ntfs_parse_mft_zero(ntfs_fs_t *fs)
{
    uint8_t *rec = (uint8_t *)kmalloc(fs->mft_record_size);
    ntfs_attr_t a;
    if (!rec) return -ENOMEM;
    if (ntfs_read_mft_record(fs, 0, rec) < 0)
        return -EIO;

    ntfs_first_attr(rec, fs->mft_record_size, &a);
    while (ntfs_next_attr(rec, fs->mft_record_size, &a) == 0) {
        if (a.type == NTFS_ATTR_DATA && a.non_resident) {
            uint8_t nlen = rec[a.attr_offset + 9];
            if (nlen == 0) {
                return ntfs_parse_attr_data(fs, rec, &a, &fs->mft_runlist);
            }
        }
        ntfs_advance_attr(&a);
    }
    return -EINVAL;
}

static int ntfs_mount(file_system_type_t *fs_type, int flags,
                      const char *dev_name, void *data, super_block_t **sb_out)
{
    const block_api_t *blk = block_api_get();
    block_device_t *bdev;
    uint8_t *boot;
    ntfs_fs_t *fs;
    ntfs_node_t *root;
    inode_t *rino;
    dentry_t *rd;
    super_block_t *sb;
    int8_t clusters_per_mft;
    int8_t clusters_per_idx;

    (void)flags; (void)data;
    if (!blk || !dev_name)
        return -EINVAL;
    bdev = blk->lookup(dev_name);
    if (!bdev)
        return -ENODEV;

    boot = (uint8_t *)kmalloc(512);
    if (!boot)
        return -ENOMEM;
    if (blk->read(bdev, 0, boot, 1) < 0)
        return -EIO;
    if (memcmp(boot + 3, "NTFS    ", 8) != 0)
        return -EINVAL;

    fs = (ntfs_fs_t *)kmalloc(sizeof(*fs));
    if (!fs)
        return -ENOMEM;
    memset(fs, 0, sizeof(*fs));
    fs->bdev                = bdev;
    fs->bytes_per_sector    = rd_le16(boot + 11);
    fs->sectors_per_cluster = boot[13];
    fs->cluster_size        = fs->bytes_per_sector * fs->sectors_per_cluster;
    fs->mft_lcn             = rd_le64(boot + 48);

    clusters_per_mft = (int8_t)boot[64];
    clusters_per_idx = (int8_t)boot[68];
    if (clusters_per_mft > 0)
        fs->mft_record_size = clusters_per_mft * fs->cluster_size;
    else
        fs->mft_record_size = 1u << (-clusters_per_mft);
    if (clusters_per_idx > 0)
        fs->index_record_size = clusters_per_idx * fs->cluster_size;
    else
        fs->index_record_size = 1u << (-clusters_per_idx);

    if (fs->mft_record_size < 256 || fs->index_record_size < 256)
        return -EINVAL;

    /* Bootstrap the $MFT's own runlist. */
    if (ntfs_parse_mft_zero(fs) < 0)
        return -EIO;

    root = (ntfs_node_t *)kmalloc(sizeof(*root));
    sb   = (super_block_t *)kmalloc(sizeof(*sb));
    if (!root || !sb)
        return -ENOMEM;
    if (ntfs_build_node(fs, NTFS_ROOT_MFT, root) < 0)
        return -EIO;

    memset(sb, 0, sizeof(*sb));
    rino = alloc_inode(sb, S_IFDIR | 0755);
    if (!rino) return -ENOMEM;
    rino->i_ino = NTFS_ROOT_MFT;
    rino->i_private = root;
    rino->i_op = &ntfs_iops;
    rino->i_fop = &ntfs_fops;

    rd = alloc_dentry("/", NULL, rino);
    if (!rd) return -ENOMEM;

    sb->s_type = fs_type;
    sb->s_root = rd;
    sb->s_bdev = bdev;
    sb->s_fs_info = fs;
    *sb_out = sb;

    vga_print("ntfs: mounted ");
    vga_print(dev_name);
    vga_print(" (cluster=");
    vga_print_uint(fs->cluster_size);
    vga_print(", mftrec=");
    vga_print_uint(fs->mft_record_size);
    vga_print(")\n");
    return 0;
}

/* ---------------- Registration ---------------- */

static file_system_type_t g_fs = {
    .name  = "ntfs",
    .mount = ntfs_mount,
};

static int ntfs_init(driver_t *drv, void *ctx)
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
    vga_print("ntfs: registered\n");
    return 0;
}

int kmod_init(void)
{
    driver_t d;
    memset(&d, 0, sizeof(d));
    strncpy(d.name,    "ntfs", DRIVER_NAME_MAX    - 1);
    strncpy(d.version, "1.0",  DRIVER_VERSION_MAX - 1);
    d.kind     = DRIVER_KIND_CUSTOM;
    d.class    = DRIVER_CLASS_FS;
    d.priority = 65;
    d.init     = ntfs_init;
    if (driver_register(&d) < 0)
        return -1;
    if (driver_load("ntfs", NULL) < 0)
        return -1;
    return 0;
}
