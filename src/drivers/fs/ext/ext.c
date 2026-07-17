/*
 * ext2/ext3/ext4 read-only filesystem driver.
 *
 * Implements superblock parse, group descriptor read (32/64-byte), inode
 * table lookup with configurable inode size, both the classic direct/
 * indirect/double-indirect/triple-indirect block map (ext2/3) and the ext4
 * extent tree (leaf + internal nodes), plus linear directory scanning
 * ("linear" htree fallback: we walk sequentially like the kernel's
 * ext4_dir_operations does for compatibility).
 *
 * Only lookup, readdir, and read are exported. Nothing here writes to
 * disk. All fields are little-endian on-disk; we treat little-endian
 * memory copies as native since the target is x86.
 */

#include <kernel/vfs_api.h>
#include <kernel/block_api.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/string.h>
#include <drivers/vfs_fs.h>
#include <drivers/driver.h>
#include <drivers/vga.h>

/* ---------------- On-disk constants ---------------- */

#define EXT_SB_OFFSET             1024u
#define EXT_SB_MAGIC              0xEF53u

#define EXT_ROOT_INO              2u
#define EXT_GOOD_OLD_INODE_SIZE   128u

/* s_feature_incompat bits */
#define EXT_FEATURE_INCOMPAT_EXTENTS 0x0040u
#define EXT_FEATURE_INCOMPAT_64BIT   0x0080u

/* inode i_flags */
#define EXT_INODE_FLAG_EXTENTS       0x00080000u

/* Directory entry file_type */
#define EXT_FT_UNKNOWN  0
#define EXT_FT_REG_FILE 1
#define EXT_FT_DIR      2
#define EXT_FT_CHRDEV   3
#define EXT_FT_BLKDEV   4
#define EXT_FT_FIFO     5
#define EXT_FT_SOCK     6
#define EXT_FT_SYMLINK  7

/* Extent header magic */
#define EXT4_EXT_MAGIC   0xF30Au

/* ---------------- Little helpers for LE reads ---------------- */

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

/* ---------------- FS types ---------------- */

typedef struct {
    block_device_t *bdev;

    uint32_t block_size;        /* 1024 << s_log_block_size */
    uint32_t inode_size;        /* s_inode_size */
    uint32_t inodes_per_group;
    uint32_t blocks_per_group;
    uint32_t first_data_block;
    uint32_t groups_count;
    uint32_t desc_size;         /* 32 or 64 */
    uint32_t feature_incompat;

    uint32_t sec_per_block;     /* block_size / 512 */
} ext_fs_t;

typedef struct {
    ext_fs_t *fs;
    uint32_t  ino;
    uint32_t  mode;
    uint64_t  size;
    uint32_t  flags;
    uint8_t   i_block[60];      /* raw copy for both indirect + extent walks */
} ext_node_t;

typedef struct {
    ext_node_t node;
    uint32_t   read_cursor;     /* used by readdir to remember position */
} ext_dir_iter_t;

static dentry_t *(*alloc_dentry)(const char *, dentry_t *, inode_t *);
static inode_t  *(*alloc_inode)(super_block_t *, uint32_t);

/* Forward decls of ops so we can link inode to fops during lookup. */
static const inode_operations_t ext_iops;
static const file_operations_t  ext_fops;

/* ---------------- Block I/O ---------------- */

static int ext_read_block(ext_fs_t *fs, uint64_t block_no, void *buf)
{
    const block_api_t *api = block_api_get();
    uint64_t lba;
    if (!api || !api->read)
        return -EIO;
    lba = block_no * fs->sec_per_block;
    return api->read(fs->bdev, lba, buf, fs->sec_per_block);
}

/* ---------------- Inode table lookup ---------------- */

static int ext_read_inode(ext_fs_t *fs, uint32_t ino, ext_node_t *out)
{
    uint8_t *desc;
    uint8_t *itab;
    uint32_t group, index_in_group;
    uint64_t desc_block;
    uint32_t desc_off;
    uint64_t inode_table_block;
    uint32_t offset_in_table;
    uint64_t inode_block_no;
    uint32_t offset_in_block;
    const uint8_t *inode_data;

    if (ino == 0)
        return -EINVAL;

    group = (ino - 1) / fs->inodes_per_group;
    index_in_group = (ino - 1) % fs->inodes_per_group;

    /*
     * Group descriptor table starts at block first_data_block+1 for
     * block_size > 1024 (superblock resides inside block 0), otherwise
     * at block first_data_block+1 = 2 (since SB is block 1 when
     * block_size == 1024). We keep first_data_block from SB to be safe.
     */
    {
        uint64_t bytes_off = (uint64_t)(fs->first_data_block + 1) * fs->block_size +
                             (uint64_t)group * fs->desc_size;
        desc_block = bytes_off / fs->block_size;
        desc_off   = (uint32_t)(bytes_off % fs->block_size);
    }

    desc = (uint8_t *)kmalloc(fs->block_size);
    if (!desc)
        return -ENOMEM;
    if (ext_read_block(fs, desc_block, desc) < 0)
        return -EIO;

    /* bg_inode_table_lo at offset 8, bg_inode_table_hi at offset 40 (only if desc_size >= 64). */
    inode_table_block = rd_le32(desc + desc_off + 8);
    if (fs->desc_size >= 64)
        inode_table_block |= ((uint64_t)rd_le32(desc + desc_off + 40)) << 32;

    offset_in_table = index_in_group * fs->inode_size;
    inode_block_no  = inode_table_block + (offset_in_table / fs->block_size);
    offset_in_block = offset_in_table % fs->block_size;

    itab = (uint8_t *)kmalloc(fs->block_size);
    if (!itab)
        return -ENOMEM;
    if (ext_read_block(fs, inode_block_no, itab) < 0)
        return -EIO;

    inode_data = itab + offset_in_block;

    out->fs    = fs;
    out->ino   = ino;
    out->mode  = rd_le16(inode_data + 0);
    out->size  = rd_le32(inode_data + 4);
    out->flags = rd_le32(inode_data + 32);

    /* i_size_hi at offset 108 is the upper 32 bits for large files. */
    if ((out->mode & 0xF000u) == 0x8000u)
        out->size |= ((uint64_t)rd_le32(inode_data + 108)) << 32;

    memcpy(out->i_block, inode_data + 40, 60);
    return 0;
}

/* ---------------- Logical → physical block mapping ---------------- */

/*
 * Walk one extent tree node. buf points to the raw node bytes (12-byte
 * header followed by entries). If depth == 0 the entries are leaf extents,
 * otherwise they are index entries.  Returns the physical block for
 * logical block `logical`, or 0 if unmapped / not found.
 */
static uint64_t ext_extent_walk(ext_fs_t *fs, const uint8_t *node, uint32_t logical)
{
    uint16_t magic   = rd_le16(node + 0);
    uint16_t entries = rd_le16(node + 2);
    uint16_t depth   = rd_le16(node + 6);
    uint16_t i;

    if (magic != EXT4_EXT_MAGIC)
        return 0;

    if (depth == 0) {
        for (i = 0; i < entries; i++) {
            const uint8_t *e = node + 12 + (size_t)i * 12;
            uint32_t ee_block  = rd_le32(e + 0);
            uint16_t ee_len    = rd_le16(e + 4);
            uint16_t ee_shi    = rd_le16(e + 6);
            uint32_t ee_slo    = rd_le32(e + 8);
            uint64_t start;

            /* Uninitialized extents have the high bit of ee_len set. */
            uint16_t len = ee_len & 0x7FFF;

            if (logical >= ee_block && logical < ee_block + len) {
                start = ((uint64_t)ee_shi << 32) | ee_slo;
                return start + (logical - ee_block);
            }
        }
        return 0;
    }

    /* Internal node — binary search style but linear is fine here. */
    {
        uint32_t idx_block = 0;
        for (i = 0; i < entries; i++) {
            const uint8_t *e = node + 12 + (size_t)i * 12;
            uint32_t ei_block = rd_le32(e + 0);
            uint32_t ei_lo    = rd_le32(e + 4);
            uint16_t ei_hi    = rd_le16(e + 8);
            if (ei_block <= logical) {
                idx_block = ei_lo | ((uint32_t)ei_hi << 16); /* only 48-bit really */
                (void)ei_hi;
            } else {
                break;
            }
        }
        if (idx_block == 0)
            return 0;
        {
            uint8_t *nb = (uint8_t *)kmalloc(fs->block_size);
            uint64_t phys;
            if (!nb)
                return 0;
            if (ext_read_block(fs, idx_block, nb) < 0)
                return 0;
            phys = ext_extent_walk(fs, nb, logical);
            return phys;
        }
    }
}

/*
 * Classic ext2 map: 12 direct + 1 indirect + 1 double + 1 triple.
 */
static uint64_t ext_indirect_map(ext_fs_t *fs, ext_node_t *n, uint32_t logical)
{
    uint32_t *pblk = (uint32_t *)n->i_block;
    uint32_t entries_per_block = fs->block_size / 4;
    uint8_t *lvl1;
    uint8_t *lvl2;
    uint8_t *lvl3;
    uint64_t out = 0;

    if (logical < 12)
        return pblk[logical];

    logical -= 12;
    if (logical < entries_per_block) {
        if (!pblk[12])
            return 0;
        lvl1 = (uint8_t *)kmalloc(fs->block_size);
        if (!lvl1)
            return 0;
        if (ext_read_block(fs, pblk[12], lvl1) < 0)
            return 0;
        out = rd_le32(lvl1 + logical * 4);
        return out;
    }

    logical -= entries_per_block;
    if (logical < entries_per_block * entries_per_block) {
        uint32_t i1 = logical / entries_per_block;
        uint32_t i2 = logical % entries_per_block;
        uint32_t bl2;
        if (!pblk[13])
            return 0;
        lvl1 = (uint8_t *)kmalloc(fs->block_size);
        if (!lvl1)
            return 0;
        if (ext_read_block(fs, pblk[13], lvl1) < 0)
            return 0;
        bl2 = rd_le32(lvl1 + i1 * 4);
        if (!bl2)
            return 0;
        lvl2 = (uint8_t *)kmalloc(fs->block_size);
        if (!lvl2)
            return 0;
        if (ext_read_block(fs, bl2, lvl2) < 0)
            return 0;
        return rd_le32(lvl2 + i2 * 4);
    }

    logical -= entries_per_block * entries_per_block;
    {
        uint32_t i1 = logical / (entries_per_block * entries_per_block);
        uint32_t rest = logical % (entries_per_block * entries_per_block);
        uint32_t i2 = rest / entries_per_block;
        uint32_t i3 = rest % entries_per_block;
        uint32_t bl2, bl3;
        if (!pblk[14])
            return 0;
        lvl1 = (uint8_t *)kmalloc(fs->block_size);
        if (!lvl1)
            return 0;
        if (ext_read_block(fs, pblk[14], lvl1) < 0)
            return 0;
        bl2 = rd_le32(lvl1 + i1 * 4);
        if (!bl2)
            return 0;
        lvl2 = (uint8_t *)kmalloc(fs->block_size);
        if (!lvl2)
            return 0;
        if (ext_read_block(fs, bl2, lvl2) < 0)
            return 0;
        bl3 = rd_le32(lvl2 + i2 * 4);
        if (!bl3)
            return 0;
        lvl3 = (uint8_t *)kmalloc(fs->block_size);
        if (!lvl3)
            return 0;
        if (ext_read_block(fs, bl3, lvl3) < 0)
            return 0;
        return rd_le32(lvl3 + i3 * 4);
    }
}

static uint64_t ext_map_block(ext_fs_t *fs, ext_node_t *n, uint32_t logical)
{
    if (n->flags & EXT_INODE_FLAG_EXTENTS)
        return ext_extent_walk(fs, n->i_block, logical);
    return ext_indirect_map(fs, n, logical);
}

/* ---------------- File read ---------------- */

static int ext_read_file(ext_node_t *n, void *buf, size_t count, off_t pos)
{
    ext_fs_t *fs = n->fs;
    uint8_t  *out = (uint8_t *)buf;
    uint8_t  *blk;
    size_t    done = 0;

    if ((uint64_t)pos >= n->size)
        return 0;
    if (count > n->size - (uint64_t)pos)
        count = (size_t)(n->size - (uint64_t)pos);

    blk = (uint8_t *)kmalloc(fs->block_size);
    if (!blk)
        return -ENOMEM;

    while (done < count) {
        uint32_t logical = (uint32_t)((uint64_t)(pos + done) / fs->block_size);
        uint32_t off     = (uint32_t)((uint64_t)(pos + done) % fs->block_size);
        uint64_t phys    = ext_map_block(fs, n, logical);
        size_t   chunk   = fs->block_size - off;

        if (chunk > count - done)
            chunk = count - done;

        if (phys == 0) {
            /* hole → return zeros */
            memset(out + done, 0, chunk);
        } else {
            if (ext_read_block(fs, phys, blk) < 0)
                return -EIO;
            memcpy(out + done, blk + off, chunk);
        }
        done += chunk;
    }
    return (int)done;
}

/* ---------------- Directory scanning ---------------- */

typedef int (*ext_dir_cb)(void *ctx,
                          uint32_t inode_no,
                          uint8_t  file_type,
                          const char *name,
                          uint8_t  name_len);

static int ext_iterate_dir(ext_node_t *dir, ext_dir_cb cb, void *ctx)
{
    ext_fs_t *fs = dir->fs;
    uint8_t  *blk;
    uint32_t total_blocks;
    uint32_t b;

    if ((dir->mode & 0xF000u) != 0x4000u)
        return -ENOTDIR;

    total_blocks = (uint32_t)((dir->size + fs->block_size - 1) / fs->block_size);
    blk = (uint8_t *)kmalloc(fs->block_size);
    if (!blk)
        return -ENOMEM;

    for (b = 0; b < total_blocks; b++) {
        uint64_t phys = ext_map_block(fs, dir, b);
        uint32_t off  = 0;
        if (!phys)
            continue;
        if (ext_read_block(fs, phys, blk) < 0)
            return -EIO;

        while (off + 8 <= fs->block_size) {
            uint32_t inode_no = rd_le32(blk + off + 0);
            uint16_t rec_len  = rd_le16(blk + off + 4);
            uint8_t  name_len = blk[off + 6];
            uint8_t  ft       = blk[off + 7];
            char     nm[256];
            int rc;

            if (rec_len < 8 || (off + rec_len) > fs->block_size)
                break;

            if (inode_no != 0 && name_len > 0 && name_len < 256) {
                memcpy(nm, blk + off + 8, name_len);
                nm[name_len] = 0;
                rc = cb(ctx, inode_no, ft, nm, name_len);
                if (rc != 0)
                    return rc;
            }
            off += rec_len;
        }
    }
    return 0;
}

typedef struct {
    const char *want;
    uint32_t    ino;
    uint8_t     ft;
    int         found;
} ext_lookup_ctx_t;

static int ext_lookup_cb(void *ctx, uint32_t ino, uint8_t ft, const char *nm, uint8_t nlen)
{
    ext_lookup_ctx_t *c = (ext_lookup_ctx_t *)ctx;
    (void)nlen;
    if (strcmp(nm, c->want) == 0) {
        c->ino = ino;
        c->ft = ft;
        c->found = 1;
        return 1; /* stop */
    }
    return 0;
}

typedef struct {
    vfs_dirent_t *out;
    size_t        max;
    size_t        written;
} ext_readdir_ctx_t;

static int ext_readdir_cb(void *ctx, uint32_t ino, uint8_t ft, const char *nm, uint8_t nlen)
{
    ext_readdir_ctx_t *c = (ext_readdir_ctx_t *)ctx;
    uint32_t type = 0;
    (void)nlen;
    if (c->written >= c->max)
        return 1;
    switch (ft) {
    case EXT_FT_REG_FILE: type = S_IFREG; break;
    case EXT_FT_DIR:      type = S_IFDIR; break;
    case EXT_FT_SYMLINK:  type = S_IFLNK; break;
    case EXT_FT_CHRDEV:   type = S_IFCHR; break;
    case EXT_FT_BLKDEV:   type = S_IFBLK; break;
    case EXT_FT_FIFO:     type = S_IFIFO; break;
    case EXT_FT_SOCK:     type = S_IFSOCK; break;
    default:              type = 0; break;
    }
    strncpy(c->out[c->written].name, nm, sizeof(c->out[c->written].name) - 1);
    c->out[c->written].name[sizeof(c->out[c->written].name) - 1] = 0;
    c->out[c->written].type = type;
    c->out[c->written].ino  = ino;
    c->written++;
    return 0;
}

/* ---------------- VFS ops ---------------- */

static ssize_t ext_f_read(file_t *file, void *buf, size_t count, off_t *pos)
{
    ext_node_t *n = (ext_node_t *)file->f_inode->i_private;
    int rc;
    if (!n)
        return -EIO;
    if ((n->mode & 0xF000u) == 0x4000u)
        return -EISDIR;
    rc = ext_read_file(n, buf, count, *pos);
    if (rc < 0)
        return rc;
    *pos += rc;
    return rc;
}

static int ext_f_readdir(file_t *file, void *dirent, size_t max)
{
    ext_node_t *n = (ext_node_t *)file->f_inode->i_private;
    ext_readdir_ctx_t ctx;
    int rc;
    ctx.out = (vfs_dirent_t *)dirent;
    ctx.max = max;
    ctx.written = 0;
    if (!n)
        return -EIO;
    rc = ext_iterate_dir(n, ext_readdir_cb, &ctx);
    if (rc < 0)
        return rc;
    return (int)ctx.written;
}

static dentry_t *ext_lookup(inode_t *dir, dentry_t *dentry)
{
    ext_node_t *dn = (ext_node_t *)dir->i_private;
    ext_lookup_ctx_t lc;
    ext_node_t *child;
    inode_t    *ino;
    dentry_t   *out;

    if (!dn)
        return NULL;
    lc.want = dentry->d_name;
    lc.found = 0;
    lc.ino = 0;
    lc.ft = 0;
    if (ext_iterate_dir(dn, ext_lookup_cb, &lc) < 0)
        return NULL;
    if (!lc.found)
        return NULL;

    child = (ext_node_t *)kmalloc(sizeof(*child));
    if (!child)
        return NULL;
    memset(child, 0, sizeof(*child));
    if (ext_read_inode(dn->fs, lc.ino, child) < 0)
        return NULL;

    ino = alloc_inode(dir->i_sb, child->mode);
    if (!ino)
        return NULL;
    ino->i_ino     = child->ino;
    ino->i_size    = child->size;
    ino->i_private = child;
    ino->i_op      = &ext_iops;
    ino->i_fop     = &ext_fops;

    out = alloc_dentry(dentry->d_name, dentry->d_parent, ino);
    return out;
}

static const inode_operations_t ext_iops = {
    .lookup = ext_lookup,
};

static const file_operations_t ext_fops = {
    .read    = ext_f_read,
    .readdir = ext_f_readdir,
};

/* ---------------- Mount ---------------- */

static int ext_mount(file_system_type_t *fs_type, int flags,
                     const char *dev_name, void *data, super_block_t **sb_out)
{
    const block_api_t *blk = block_api_get();
    block_device_t *bdev;
    uint8_t   *sb_buf;
    ext_fs_t  *fs;
    ext_node_t *root;
    inode_t   *rino;
    dentry_t  *rd;
    super_block_t *sb;

    (void)flags;
    (void)data;
    if (!blk || !dev_name)
        return -EINVAL;
    bdev = blk->lookup(dev_name);
    if (!bdev)
        return -ENODEV;

    /* Superblock lives 2 sectors from the start of the volume. */
    sb_buf = (uint8_t *)kmalloc(1024);
    if (!sb_buf)
        return -ENOMEM;
    if (blk->read(bdev, EXT_SB_OFFSET / 512, sb_buf, 2) < 0)
        return -EIO;
    if (rd_le16(sb_buf + 56) != EXT_SB_MAGIC)
        return -EINVAL;

    fs = (ext_fs_t *)kmalloc(sizeof(*fs));
    if (!fs)
        return -ENOMEM;
    memset(fs, 0, sizeof(*fs));
    fs->bdev            = bdev;
    fs->block_size      = 1024u << rd_le32(sb_buf + 24);
    fs->inodes_per_group = rd_le32(sb_buf + 40);
    fs->blocks_per_group = rd_le32(sb_buf + 32);
    fs->first_data_block = rd_le32(sb_buf + 20);
    fs->feature_incompat = rd_le32(sb_buf + 96);
    fs->sec_per_block   = fs->block_size / 512u;

    /* inode size: 128 for rev 0, else s_inode_size */
    if (rd_le32(sb_buf + 76) == 0)
        fs->inode_size = EXT_GOOD_OLD_INODE_SIZE;
    else
        fs->inode_size = rd_le16(sb_buf + 88);
    if (fs->inode_size < 128)
        fs->inode_size = 128;

    /* Descriptor size: default 32 unless 64BIT feature set. */
    if (fs->feature_incompat & EXT_FEATURE_INCOMPAT_64BIT)
        fs->desc_size = rd_le16(sb_buf + 254);
    if (fs->desc_size < 32)
        fs->desc_size = 32;

    {
        uint64_t total_blocks = rd_le32(sb_buf + 4);
        fs->groups_count = (uint32_t)((total_blocks + fs->blocks_per_group - 1) /
                                      fs->blocks_per_group);
    }

    root = (ext_node_t *)kmalloc(sizeof(*root));
    sb   = (super_block_t *)kmalloc(sizeof(*sb));
    if (!root || !sb)
        return -ENOMEM;
    memset(root, 0, sizeof(*root));
    memset(sb, 0, sizeof(*sb));
    if (ext_read_inode(fs, EXT_ROOT_INO, root) < 0)
        return -EIO;

    rino = alloc_inode(sb, root->mode);
    if (!rino)
        return -ENOMEM;
    rino->i_ino     = EXT_ROOT_INO;
    rino->i_size    = root->size;
    rino->i_private = root;
    rino->i_op      = &ext_iops;
    rino->i_fop     = &ext_fops;

    rd = alloc_dentry("/", NULL, rino);
    if (!rd)
        return -ENOMEM;

    sb->s_type    = fs_type;
    sb->s_root    = rd;
    sb->s_bdev    = bdev;
    sb->s_fs_info = fs;
    *sb_out       = sb;

    vga_print("ext4: mounted ");
    vga_print(dev_name);
    vga_print(" (blk=");
    vga_print_uint(fs->block_size);
    vga_print(", inosz=");
    vga_print_uint(fs->inode_size);
    vga_print(")\n");
    return 0;
}

/* ---------------- Registration ---------------- */

static file_system_type_t g_fs = {
    .name  = "ext4",
    .mount = ext_mount,
};

static file_system_type_t g_fs2 = {
    .name  = "ext2",
    .mount = ext_mount,
};

static file_system_type_t g_fs3 = {
    .name  = "ext3",
    .mount = ext_mount,
};

static int ext_init(driver_t *drv, void *ctx)
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
    (void)api->register_filesystem(&g_fs2);
    (void)api->register_filesystem(&g_fs3);
    vga_print("ext: ext2/ext3/ext4 registered\n");
    return 0;
}

int kmod_init(void)
{
    driver_t d;
    memset(&d, 0, sizeof(d));
    strncpy(d.name,    "ext",  DRIVER_NAME_MAX    - 1);
    strncpy(d.version, "1.0",  DRIVER_VERSION_MAX - 1);
    d.kind     = DRIVER_KIND_CUSTOM;
    d.class    = DRIVER_CLASS_FS;
    d.priority = 65;
    d.init     = ext_init;
    if (driver_register(&d) < 0)
        return -1;
    if (driver_load("ext", NULL) < 0)
        return -1;
    return 0;
}
