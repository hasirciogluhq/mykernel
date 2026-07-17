#include <kernel/vfs_api.h>
#include <kernel/block_api.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/string.h>
#include <drivers/vfs_fs.h>
#include <drivers/driver.h>
#include <drivers/vga.h>

static dentry_t *(*alloc_dentry)(const char *, dentry_t *, inode_t *);
static inode_t *(*alloc_inode)(super_block_t *, uint32_t);

typedef struct {
    block_device_t *bdev;
    uint16_t bytes_per_sec;
    uint8_t  sec_per_clust;
    uint16_t reserved;
    uint8_t  fats;
    uint32_t fat_sectors;
    uint32_t root_clust; /* FAT32 */
    uint32_t data_start;
    uint32_t root_ents;  /* FAT12/16 */
    int      fat_bits;   /* 12/16/32 */
    uint32_t clusters;
} fat_fs_t;

typedef struct {
    fat_fs_t *fs;
    uint32_t  first_clust;
    uint32_t  size;
    int       is_dir;
} fat_node_t;

static int fat_read_sector(fat_fs_t *fs, uint32_t lba, void *buf)
{
    const block_api_t *api = block_api_get();
    if (!api || !api->read)
        return -EIO;
    return api->read(fs->bdev, lba, buf, 1);
}

static uint32_t clust_to_lba(fat_fs_t *fs, uint32_t cl)
{
    return fs->data_start + (cl - 2) * fs->sec_per_clust;
}

static uint32_t fat_next(fat_fs_t *fs, uint32_t cl)
{
    uint8_t *sec = (uint8_t *)kmalloc(512);
    uint32_t fat_off, lba, ent;
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
        if (cl & 1)
            ent = w >> 4;
        else
            ent = w & 0x0FFF;
    }
    return ent;
}

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
    if (!sec)
        return -ENOMEM;

    while (cl >= 2 && cl < 0x0FFFFFF8u && done < count) {
        uint32_t i;
        for (i = 0; i < fs->sec_per_clust && done < count; i++) {
            uint32_t lba = clust_to_lba(fs, cl) + i;
            size_t off = 0;
            if (fat_read_sector(fs, lba, sec) < 0)
                return -EIO;
            if (skip >= 512) {
                skip -= 512;
                continue;
            }
            off = skip;
            skip = 0;
            {
                size_t take = 512 - off;
                if (take > count - done)
                    take = count - done;
                memcpy(out + done, sec + off, take);
                done += take;
            }
        }
        cl = fat_next(fs, cl);
    }
    return (int)done;
}

static ssize_t fat_f_read(file_t *file, void *buf, size_t count, off_t *pos)
{
    fat_node_t *n = (fat_node_t *)file->f_inode->i_private;
    int rc;
    if (!n || n->is_dir)
        return -EISDIR;
    rc = fat_read_file(n, buf, count, *pos);
    if (rc < 0)
        return rc;
    *pos += rc;
    return rc;
}

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
    /* lowercase-ish for lookup convenience */
    for (i = 0; out[i]; i++) {
        if (out[i] >= 'A' && out[i] <= 'Z')
            out[i] = (char)(out[i] - 'A' + 'a');
    }
}

static int name_eq(const char *a, const char *b)
{
    char ca, cb;
    while (*a && *b) {
        ca = *a;
        cb = *b;
        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb - 'A' + 'a');
        if (ca != cb)
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static fat_node_t *fat_find_in_dir(fat_fs_t *fs, uint32_t dir_clust, const char *name,
                                   int *is_dir_out, uint32_t *size_out, uint32_t *clust_out)
{
    uint8_t *sec = (uint8_t *)kmalloc(512);
    uint32_t cl = dir_clust ? dir_clust : fs->root_clust;
    if (!sec)
        return NULL;

    /* FAT12/16 root is contiguous */
    if (fs->fat_bits != 32 && dir_clust == 0) {
        uint32_t root_secs = (fs->root_ents * 32 + fs->bytes_per_sec - 1) / fs->bytes_per_sec;
        uint32_t i, e;
        uint32_t root_lba = fs->reserved + fs->fats * fs->fat_sectors;
        for (i = 0; i < root_secs; i++) {
            if (fat_read_sector(fs, root_lba + i, sec) < 0)
                return NULL;
            for (e = 0; e < 512; e += 32) {
                char nm[16];
                if (sec[e] == 0)
                    return NULL;
                if (sec[e] == 0xE5 || (sec[e + 11] & 0x08))
                    continue;
                fat83_to_name(sec + e, nm);
                if (name_eq(nm, name)) {
                    uint16_t lo = *(uint16_t *)(sec + e + 26);
                    uint16_t hi = *(uint16_t *)(sec + e + 20);
                    *clust_out = ((uint32_t)hi << 16) | lo;
                    *size_out = *(uint32_t *)(sec + e + 28);
                    *is_dir_out = (sec[e + 11] & 0x10) != 0;
                    return (fat_node_t *)1; /* non-null sentinel */
                }
            }
        }
        return NULL;
    }

    while (cl >= 2 && cl < 0x0FFFFFF8u) {
        uint32_t s, e;
        for (s = 0; s < fs->sec_per_clust; s++) {
            if (fat_read_sector(fs, clust_to_lba(fs, cl) + s, sec) < 0)
                return NULL;
            for (e = 0; e < 512; e += 32) {
                char nm[16];
                if (sec[e] == 0)
                    return NULL;
                if (sec[e] == 0xE5 || (sec[e + 11] & 0x08))
                    continue;
                fat83_to_name(sec + e, nm);
                if (name_eq(nm, name)) {
                    uint16_t lo = *(uint16_t *)(sec + e + 26);
                    uint16_t hi = *(uint16_t *)(sec + e + 20);
                    *clust_out = ((uint32_t)hi << 16) | lo;
                    *size_out = *(uint32_t *)(sec + e + 28);
                    *is_dir_out = (sec[e + 11] & 0x10) != 0;
                    return (fat_node_t *)1;
                }
            }
        }
        cl = fat_next(fs, cl);
    }
    return NULL;
}

static dentry_t *fat_lookup(inode_t *dir, dentry_t *dentry)
{
    fat_node_t *dn = (fat_node_t *)dir->i_private;
    fat_fs_t *fs;
    int is_dir = 0;
    uint32_t size = 0, cl = 0;
    fat_node_t *nn;
    inode_t *ino;
    dentry_t *out;

    if (!dn)
        return NULL;
    fs = dn->fs;
    if (!fat_find_in_dir(fs, dn->first_clust, dentry->d_name, &is_dir, &size, &cl))
        return NULL;

    nn = (fat_node_t *)kmalloc(sizeof(*nn));
    if (!nn)
        return NULL;
    nn->fs = fs;
    nn->first_clust = cl;
    nn->size = size;
    nn->is_dir = is_dir;

    ino = alloc_inode(dir->i_sb, is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644));
    if (!ino)
        return NULL;
    ino->i_size = size;
    ino->i_private = nn;
    ino->i_op = dir->i_op;
    ino->i_fop = dir->i_fop;

    out = alloc_dentry(dentry->d_name, dentry->d_parent, ino);
    return out;
}

static const file_operations_t fat_fops = {
    .read = fat_f_read,
};

static const inode_operations_t fat_iops = {
    .lookup = fat_lookup,
};

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
    if (!bpb)
        return -ENOMEM;
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

    fatsz = fatsz16 ? fatsz16 : fatsz32;
    totsec = tot16 ? tot16 : tot32;
    data_secs = totsec - (rsvd + nfats * fatsz +
                          ((root_ents * 32) + (bps - 1)) / bps);
    clusters = data_secs / spc;

    fs = (fat_fs_t *)kmalloc(sizeof(*fs));
    if (!fs)
        return -ENOMEM;
    memset(fs, 0, sizeof(*fs));
    fs->bdev = bdev;
    fs->bytes_per_sec = bps;
    fs->sec_per_clust = spc;
    fs->reserved = rsvd;
    fs->fats = nfats;
    fs->fat_sectors = fatsz;
    fs->root_ents = root_ents;
    fs->clusters = clusters;
    if (clusters < 4085)
        fs->fat_bits = 12;
    else if (clusters < 65525)
        fs->fat_bits = 16;
    else
        fs->fat_bits = 32;
    fs->root_clust = fs->fat_bits == 32 ? *(uint32_t *)(bpb + 44) : 0;
    fs->data_start = rsvd + nfats * fatsz +
                     ((root_ents * 32) + (bps - 1)) / bps;

    sb = (super_block_t *)kmalloc(sizeof(*sb));
    root = (fat_node_t *)kmalloc(sizeof(*root));
    if (!sb || !root)
        return -ENOMEM;
    memset(sb, 0, sizeof(*sb));
    memset(root, 0, sizeof(*root));
    root->fs = fs;
    root->first_clust = fs->root_clust;
    root->is_dir = 1;

    rino = alloc_inode(sb, S_IFDIR | 0755);
    if (!rino)
        return -ENOMEM;
    rino->i_op = &fat_iops;
    rino->i_fop = &fat_fops;
    rino->i_private = root;

    rd = alloc_dentry("/", NULL, rino);
    if (!rd)
        return -ENOMEM;

    sb->s_type = fs_type;
    sb->s_root = rd;
    sb->s_bdev = bdev;
    sb->s_fs_info = fs;
    *sb_out = sb;
    vga_print("fat: mounted ");
    vga_print(dev_name);
    vga_print("\n");
    return 0;
}

static file_system_type_t g_fat = {
    .name = "fat",
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
    return api->register_filesystem(&g_fat);
}

int kmod_init(void)
{
    driver_t d;
    memset(&d, 0, sizeof(d));
    strncpy(d.name, "fat", DRIVER_NAME_MAX - 1);
    strncpy(d.version, "1.0", DRIVER_VERSION_MAX - 1);
    d.kind = DRIVER_KIND_CUSTOM;
    d.class = DRIVER_CLASS_MISC;
    d.priority = 60;
    d.init = fat_init;
    if (driver_register(&d) < 0)
        return -1;
    if (driver_load("fat", NULL) < 0)
        return -1;
    return 0;
}
