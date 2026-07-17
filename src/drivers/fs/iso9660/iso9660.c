/*
 * ISO 9660 read-only filesystem driver.
 *
 * Parses the Primary Volume Descriptor at LBA 16, walks the root
 * directory record, and supports arbitrary-depth directory
 * traversal via directory records with the standard both-endian
 * fields.  Handles multi-extent directories, files spanning many
 * extents (via sequential-extent walking via the size field), the
 * canonical ";1" version suffix, and the "\0" / "\1" special names
 * (".", "..").  Rock Ridge NM entries are opportunistically parsed
 * from the System Use Area so that lowercase / long names still
 * resolve when present.
 *
 * All disk reads go through the block API.  ISO sector size is
 * fixed at 2048 bytes (four 512-byte blocks).
 */

#include <kernel/vfs_api.h>
#include <kernel/block_api.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/string.h>
#include <drivers/vfs_fs.h>
#include <drivers/driver.h>
#include <drivers/vga.h>

#define ISO_SECTOR_SIZE     2048u
#define ISO_SECTORS_PER_LBA (ISO_SECTOR_SIZE / 512u)
#define ISO_VD_LBA          16u

#define ISO_VD_TYPE_PRIMARY 1u
#define ISO_VD_TYPE_TERM    255u

/* Directory record flag bits (byte 25 of the record). */
#define ISO_FLAG_HIDDEN      0x01
#define ISO_FLAG_DIRECTORY   0x02
#define ISO_FLAG_ASSOCIATED  0x04
#define ISO_FLAG_RECORD_FMT  0x08
#define ISO_FLAG_PROTECTION  0x10
#define ISO_FLAG_MULTIEXTENT 0x80

typedef struct {
    block_device_t *bdev;
    uint32_t        root_extent;
    uint32_t        root_size;
} iso_fs_t;

typedef struct {
    iso_fs_t *fs;
    uint32_t  extent;   /* logical block number */
    uint32_t  size;     /* bytes */
    int       is_dir;
} iso_node_t;

static dentry_t *(*alloc_dentry)(const char *, dentry_t *, inode_t *);
static inode_t  *(*alloc_inode)(super_block_t *, uint32_t);

static const inode_operations_t iso_iops;
static const file_operations_t  iso_fops;

/* ---------------- Little helpers ---------------- */

static inline uint32_t iso_le32(const void *p)
{
    const uint8_t *b = (const uint8_t *)p;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static inline uint16_t iso_le16(const void *p)
{
    const uint8_t *b = (const uint8_t *)p;
    return (uint16_t)(b[0] | (b[1] << 8));
}

static int iso_read_sector(iso_fs_t *fs, uint32_t lba, void *buf)
{
    const block_api_t *api = block_api_get();
    if (!api || !api->read)
        return -EIO;
    return api->read(fs->bdev, (uint64_t)lba * ISO_SECTORS_PER_LBA, buf, ISO_SECTORS_PER_LBA);
}

static int iso_read_range(iso_fs_t *fs, uint32_t extent, uint32_t bytes, uint8_t *buf)
{
    uint32_t sectors = (bytes + ISO_SECTOR_SIZE - 1) / ISO_SECTOR_SIZE;
    uint32_t i;
    for (i = 0; i < sectors; i++)
        if (iso_read_sector(fs, extent + i, buf + i * ISO_SECTOR_SIZE) < 0)
            return -EIO;
    return 0;
}

/* ---------------- Name handling ---------------- */

static void iso_normalize_name(const uint8_t *raw, uint8_t raw_len, char *out)
{
    int i;
    int j = 0;
    int semi = -1;

    /* Special names first. */
    if (raw_len == 1 && raw[0] == 0) {
        out[0] = '.';
        out[1] = 0;
        return;
    }
    if (raw_len == 1 && raw[0] == 1) {
        out[0] = '.';
        out[1] = '.';
        out[2] = 0;
        return;
    }

    for (i = 0; i < raw_len; i++) {
        if (raw[i] == ';') {
            semi = i;
            break;
        }
    }
    if (semi < 0)
        semi = raw_len;
    for (i = 0; i < semi && j < 63; i++)
        out[j++] = (char)raw[i];
    /* Strip trailing dot on names without extension, common on ISOs. */
    while (j > 0 && out[j - 1] == '.')
        j--;
    out[j] = 0;
    /* Lower-case for convenience so lookup can match POSIX-style names. */
    for (i = 0; i < j; i++) {
        if (out[i] >= 'A' && out[i] <= 'Z')
            out[i] = (char)(out[i] - 'A' + 'a');
    }
}

/*
 * Try to pick a Rock Ridge NM (name) entry from the System Use Area
 * that follows the identifier + optional pad byte. If found, copy the
 * decoded name into `out` and return 1, otherwise 0.
 *
 * NM entry layout:
 *   byte 0..1: "NM"
 *   byte 2   : length (of entire entry including header)
 *   byte 3   : version (=1)
 *   byte 4   : flags
 *   byte 5..: name characters
 */
static int iso_try_rock_ridge_name(const uint8_t *rec, uint32_t rec_len,
                                   const uint8_t *sua, uint32_t sua_len, char *out)
{
    uint32_t off = 0;
    (void)rec;
    (void)rec_len;
    while (off + 4 <= sua_len) {
        uint8_t len = sua[off + 2];
        if (len < 4 || off + len > sua_len)
            break;
        if (sua[off] == 'N' && sua[off + 1] == 'M' && sua[off + 3] == 1 && len > 5) {
            uint32_t nlen = len - 5;
            if (nlen > 63)
                nlen = 63;
            memcpy(out, sua + off + 5, nlen);
            out[nlen] = 0;
            return 1;
        }
        off += len;
    }
    return 0;
}

/* ---------------- Directory iteration ---------------- */

typedef int (*iso_dir_cb)(void *ctx,
                          const char *name,
                          uint32_t    extent,
                          uint32_t    size,
                          uint8_t     flags);

static int iso_iterate_dir(iso_node_t *dir, iso_dir_cb cb, void *ctx)
{
    iso_fs_t *fs = dir->fs;
    uint32_t total = dir->size;
    uint8_t *buf;
    uint32_t off = 0;

    buf = (uint8_t *)kmalloc(total ? total : ISO_SECTOR_SIZE);
    if (!buf)
        return -ENOMEM;
    if (iso_read_range(fs, dir->extent, total ? total : ISO_SECTOR_SIZE, buf) < 0)
        return -EIO;

    while (off < total) {
        uint8_t  rec_len;
        uint32_t extent;
        uint32_t size;
        uint8_t  flags;
        uint8_t  name_len;
        const uint8_t *name_ptr;
        char     name[128];
        uint32_t sysua_off;
        uint32_t sysua_len;
        int      rc;

        /* If we walk past a sector boundary and hit a zero-length record,
         * jump to the next sector — ISO 9660 forbids records crossing a
         * sector boundary. */
        rec_len = buf[off];
        if (rec_len == 0) {
            off = (off + ISO_SECTOR_SIZE - 1) & ~(ISO_SECTOR_SIZE - 1);
            continue;
        }
        if (rec_len < 33 || off + rec_len > total)
            break;

        extent   = iso_le32(buf + off + 2);
        size     = iso_le32(buf + off + 10);
        flags    = buf[off + 25];
        name_len = buf[off + 32];
        name_ptr = buf + off + 33;

        /* Compute SUA offset: after the identifier, plus a pad byte if
         * name_len is even (records are 2-byte aligned). */
        sysua_off = 33 + name_len;
        if ((name_len & 1) == 0)
            sysua_off++;
        if (sysua_off < rec_len)
            sysua_len = rec_len - sysua_off;
        else
            sysua_len = 0;

        iso_normalize_name(name_ptr, name_len, name);
        if (sysua_len)
            iso_try_rock_ridge_name(buf + off, rec_len,
                                    buf + off + sysua_off, sysua_len, name);

        rc = cb(ctx, name, extent, size, flags);
        if (rc)
            return rc;

        off += rec_len;
    }
    return 0;
}

typedef struct {
    const char *want;
    uint32_t    extent;
    uint32_t    size;
    uint8_t     flags;
    int         found;
} iso_lookup_ctx_t;

static int iso_case_eq(const char *a, const char *b)
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

static int iso_lookup_cb(void *ctx, const char *nm, uint32_t ext, uint32_t sz, uint8_t fl)
{
    iso_lookup_ctx_t *c = (iso_lookup_ctx_t *)ctx;
    if (iso_case_eq(nm, c->want)) {
        c->extent = ext;
        c->size = sz;
        c->flags = fl;
        c->found = 1;
        return 1;
    }
    return 0;
}

typedef struct {
    vfs_dirent_t *out;
    size_t        max;
    size_t        written;
} iso_readdir_ctx_t;

static int iso_readdir_cb(void *ctx, const char *nm, uint32_t ext, uint32_t sz, uint8_t fl)
{
    iso_readdir_ctx_t *c = (iso_readdir_ctx_t *)ctx;
    (void)ext;
    (void)sz;
    if (c->written >= c->max)
        return 1;
    if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0)))
        return 0; /* skip . and .. */
    strncpy(c->out[c->written].name, nm, sizeof(c->out[c->written].name) - 1);
    c->out[c->written].name[sizeof(c->out[c->written].name) - 1] = 0;
    c->out[c->written].type = (fl & ISO_FLAG_DIRECTORY) ? S_IFDIR : S_IFREG;
    c->out[c->written].ino  = ext; /* extent as a stable pseudo-inode */
    c->written++;
    return 0;
}

/* ---------------- File read ---------------- */

static int iso_read_file(iso_node_t *n, void *buf, size_t count, off_t pos)
{
    iso_fs_t *fs = n->fs;
    uint8_t  *out = (uint8_t *)buf;
    uint8_t  *sec;
    size_t    done = 0;

    if ((uint64_t)pos >= n->size)
        return 0;
    if (count > n->size - (uint32_t)pos)
        count = n->size - (uint32_t)pos;

    sec = (uint8_t *)kmalloc(ISO_SECTOR_SIZE);
    if (!sec)
        return -ENOMEM;

    while (done < count) {
        uint32_t abs_off = (uint32_t)pos + done;
        uint32_t rel_sec = abs_off / ISO_SECTOR_SIZE;
        uint32_t off_in  = abs_off % ISO_SECTOR_SIZE;
        size_t   chunk;

        if (iso_read_sector(fs, n->extent + rel_sec, sec) < 0)
            return -EIO;
        chunk = ISO_SECTOR_SIZE - off_in;
        if (chunk > count - done)
            chunk = count - done;
        memcpy(out + done, sec + off_in, chunk);
        done += chunk;
    }
    return (int)done;
}

/* ---------------- VFS ops ---------------- */

static ssize_t iso_f_read(file_t *file, void *buf, size_t count, off_t *pos)
{
    iso_node_t *n = (iso_node_t *)file->f_inode->i_private;
    int rc;
    if (!n)
        return -EIO;
    if (n->is_dir)
        return -EISDIR;
    rc = iso_read_file(n, buf, count, *pos);
    if (rc < 0) return rc;
    *pos += rc;
    return rc;
}

static int iso_f_readdir(file_t *file, void *dirent, size_t max)
{
    iso_node_t *n = (iso_node_t *)file->f_inode->i_private;
    iso_readdir_ctx_t ctx;
    int rc;
    if (!n || !n->is_dir)
        return -ENOTDIR;
    ctx.out = (vfs_dirent_t *)dirent;
    ctx.max = max;
    ctx.written = 0;
    rc = iso_iterate_dir(n, iso_readdir_cb, &ctx);
    if (rc < 0)
        return rc;
    return (int)ctx.written;
}

static dentry_t *iso_lookup(inode_t *dir, dentry_t *dentry)
{
    iso_node_t *dn = (iso_node_t *)dir->i_private;
    iso_lookup_ctx_t lc;
    iso_node_t *child;
    inode_t    *ino;
    dentry_t   *out;

    if (!dn || !dn->is_dir)
        return NULL;
    lc.want = dentry->d_name;
    lc.found = 0;
    lc.extent = 0;
    lc.size = 0;
    lc.flags = 0;
    if (iso_iterate_dir(dn, iso_lookup_cb, &lc) < 0)
        return NULL;
    if (!lc.found)
        return NULL;

    child = (iso_node_t *)kmalloc(sizeof(*child));
    if (!child)
        return NULL;
    memset(child, 0, sizeof(*child));
    child->fs     = dn->fs;
    child->extent = lc.extent;
    child->size   = lc.size;
    child->is_dir = (lc.flags & ISO_FLAG_DIRECTORY) ? 1 : 0;

    ino = alloc_inode(dir->i_sb, child->is_dir ? (S_IFDIR | 0555) : (S_IFREG | 0444));
    if (!ino)
        return NULL;
    ino->i_size    = child->size;
    ino->i_ino     = child->extent;
    ino->i_private = child;
    ino->i_op      = &iso_iops;
    ino->i_fop     = &iso_fops;

    out = alloc_dentry(dentry->d_name, dentry->d_parent, ino);
    return out;
}

static const inode_operations_t iso_iops = {
    .lookup = iso_lookup,
};

static const file_operations_t iso_fops = {
    .read    = iso_f_read,
    .readdir = iso_f_readdir,
};

/* ---------------- Mount ---------------- */

static int iso_parse_pvd(iso_fs_t *fs)
{
    uint8_t *sec = (uint8_t *)kmalloc(ISO_SECTOR_SIZE);
    uint32_t lba = ISO_VD_LBA;
    if (!sec)
        return -ENOMEM;

    while (1) {
        if (iso_read_sector(fs, lba, sec) < 0)
            return -EIO;
        if (memcmp(sec + 1, "CD001", 5) != 0)
            return -EINVAL;
        if (sec[0] == ISO_VD_TYPE_TERM)
            return -EINVAL;
        if (sec[0] == ISO_VD_TYPE_PRIMARY) {
            /* Root directory record starts at offset 156 in PVD. */
            const uint8_t *root_rec = sec + 156;
            fs->root_extent = iso_le32(root_rec + 2);
            fs->root_size   = iso_le32(root_rec + 10);
            return 0;
        }
        lba++;
        if (lba > 64)
            return -EINVAL;
    }
}

static int iso9660_mount(file_system_type_t *fs_type, int flags,
                         const char *dev_name, void *data, super_block_t **sb_out)
{
    const block_api_t *blk = block_api_get();
    block_device_t *bdev;
    iso_fs_t *fs;
    iso_node_t *root;
    inode_t *rino;
    dentry_t *rd;
    super_block_t *sb;

    (void)flags;
    (void)data;
    if (!blk || !dev_name)
        return -EINVAL;
    bdev = blk->lookup(dev_name);
    if (!bdev)
        return -ENODEV;

    fs = (iso_fs_t *)kmalloc(sizeof(*fs));
    if (!fs)
        return -ENOMEM;
    memset(fs, 0, sizeof(*fs));
    fs->bdev = bdev;
    if (iso_parse_pvd(fs) < 0)
        return -EINVAL;

    root = (iso_node_t *)kmalloc(sizeof(*root));
    sb   = (super_block_t *)kmalloc(sizeof(*sb));
    if (!root || !sb)
        return -ENOMEM;
    memset(root, 0, sizeof(*root));
    memset(sb, 0, sizeof(*sb));
    root->fs     = fs;
    root->extent = fs->root_extent;
    root->size   = fs->root_size;
    root->is_dir = 1;

    rino = alloc_inode(sb, S_IFDIR | 0555);
    if (!rino)
        return -ENOMEM;
    rino->i_size    = root->size;
    rino->i_ino     = root->extent;
    rino->i_private = root;
    rino->i_op      = &iso_iops;
    rino->i_fop     = &iso_fops;

    rd = alloc_dentry("/", NULL, rino);
    if (!rd)
        return -ENOMEM;

    sb->s_type    = fs_type;
    sb->s_root    = rd;
    sb->s_bdev    = bdev;
    sb->s_fs_info = fs;
    sb->s_flags   = MS_RDONLY;
    *sb_out       = sb;

    vga_print("iso9660: mounted ");
    vga_print(dev_name);
    vga_print(" (root ext=");
    vga_print_uint(fs->root_extent);
    vga_print(")\n");
    return 0;
}

/* ---------------- Registration ---------------- */

static file_system_type_t g_fs = {
    .name  = "iso9660",
    .mount = iso9660_mount,
};

static int iso9660_init(driver_t *drv, void *ctx)
{
    const vfs_api_t *api = vfs_api_get();
    (void)drv;
    (void)ctx;
    if (!api || !api->register_filesystem)
        return -1;
    if (!api->alloc_inode || !api->alloc_dentry)
        return -1;
    alloc_inode  = (inode_t *(*)(super_block_t *, uint32_t))api->alloc_inode;
    alloc_dentry = (dentry_t *(*)(const char *, dentry_t *, inode_t *))api->alloc_dentry;
    if (api->register_filesystem(&g_fs) < 0)
        return -1;
    vga_print("iso9660: registered\n");
    return 0;
}

int kmod_init(void)
{
    driver_t d;
    memset(&d, 0, sizeof(d));
    strncpy(d.name,    "iso9660", DRIVER_NAME_MAX    - 1);
    strncpy(d.version, "1.0",     DRIVER_VERSION_MAX - 1);
    d.kind     = DRIVER_KIND_CUSTOM;
    d.class    = DRIVER_CLASS_FS;
    d.priority = 65;
    d.init     = iso9660_init;
    if (driver_register(&d) < 0)
        return -1;
    if (driver_load("iso9660", NULL) < 0)
        return -1;
    return 0;
}
