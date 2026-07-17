#include <kernel/vfs_api.h>
#include <kernel/block_api.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/string.h>
#include <drivers/vfs_fs.h>
#include <drivers/driver.h>
#include <drivers/vga.h>

#define SYSFS_MAX_DISKS 32

typedef enum { SYS_ROOT, SYS_BLOCK, SYS_DISK, SYS_SIZE } sys_kind_t;
typedef struct sys_node {
    char name[32];
    sys_kind_t kind;
    block_device_t *disk;
    inode_t *inode;
} sys_node_t;

static inode_t *(*alloc_inode)(super_block_t *, uint32_t);
static dentry_t *(*alloc_dentry)(const char *, dentry_t *, inode_t *);
static sys_node_t g_root = { "/", SYS_ROOT, NULL, NULL };
static sys_node_t g_block = { "block", SYS_BLOCK, NULL, NULL };
static sys_node_t g_disks[SYSFS_MAX_DISKS];
static sys_node_t g_sizes[SYSFS_MAX_DISKS];
static size_t g_ndisks;

static size_t decimal(char *buf, uint64_t v)
{
    char tmp[24]; size_t n = 0, i;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (i = 0; i < n; i++) buf[i] = tmp[n - i - 1];
    return n;
}

static ssize_t sys_read(file_t *file, void *buf, size_t count, off_t *pos)
{
    sys_node_t *node = file->f_inode->i_private;
    char text[32]; size_t len;
    if (!node || node->kind != SYS_SIZE) return -EISDIR;
    len = decimal(text, node->disk ? node->disk->capacity : 0);
    text[len++] = '\n';
    if (*pos < 0 || (size_t)*pos >= len) return 0;
    if (count > len - (size_t)*pos) count = len - (size_t)*pos;
    memcpy(buf, text + *pos, count); *pos += (off_t)count;
    return (ssize_t)count;
}

static dentry_t *sys_lookup(inode_t *dir, dentry_t *dentry)
{
    sys_node_t *node = dir->i_private; size_t i;
    if (!node) return NULL;
    if (node->kind == SYS_ROOT && strcmp(dentry->d_name, "block") == 0)
        return alloc_dentry("block", dentry->d_parent, g_block.inode);
    if (node->kind == SYS_BLOCK)
        for (i = 0; i < g_ndisks; i++)
            if (strcmp(g_disks[i].name, dentry->d_name) == 0)
                return alloc_dentry(dentry->d_name, dentry->d_parent, g_disks[i].inode);
    if (node->kind == SYS_DISK && strcmp(dentry->d_name, "size") == 0)
        for (i = 0; i < g_ndisks; i++)
            if (g_disks[i].disk == node->disk)
                return alloc_dentry("size", dentry->d_parent, g_sizes[i].inode);
    return NULL;
}

static int sys_readdir(file_t *file, void *dirent, size_t max)
{
    sys_node_t *node = file->f_inode->i_private; vfs_dirent_t *out = dirent;
    size_t i, n = 0;
    if (!node || (node->kind != SYS_ROOT && node->kind != SYS_BLOCK && node->kind != SYS_DISK))
        return -ENOTDIR;
    if (!max) return 0;
    if (node->kind == SYS_ROOT) {
        strcpy(out[0].name, "block"); out[0].ino = g_block.inode->i_ino; out[0].type = S_IFDIR; return 1;
    }
    if (node->kind == SYS_DISK) {
        strcpy(out[0].name, "size");
        for (i = 0; i < g_ndisks; i++) if (g_disks[i].disk == node->disk) out[0].ino = g_sizes[i].inode->i_ino;
        out[0].type = S_IFREG; return 1;
    }
    for (i = 0; i < g_ndisks && n < max; i++) {
        strcpy(out[n].name, g_disks[i].name); out[n].ino = g_disks[i].inode->i_ino; out[n].type = S_IFDIR; n++;
    }
    return (int)n;
}

static const inode_operations_t sys_iops = { .lookup = sys_lookup };
static const file_operations_t sys_fops = { .read = sys_read, .readdir = sys_readdir };

static int sysfs_mount(file_system_type_t *fs_type, int flags,
                       const char *dev_name, void *data, super_block_t **sb_out)
{
    const block_api_t *blk = block_api_get();
    super_block_t *sb; dentry_t *rd; size_t i;
    (void)flags; (void)dev_name; (void)data;
    if (!blk || !blk->disk_count || !blk->disk_at) return -ENODEV;
    sb = kmalloc(sizeof(*sb)); if (!sb) return -ENOMEM;
    memset(sb, 0, sizeof(*sb)); sb->s_type = fs_type;
    g_ndisks = blk->disk_count(); if (g_ndisks > SYSFS_MAX_DISKS) g_ndisks = SYSFS_MAX_DISKS;
    g_root.inode = alloc_inode(sb, S_IFDIR | 0555); g_block.inode = alloc_inode(sb, S_IFDIR | 0555);
    if (!g_root.inode || !g_block.inode) return -ENOMEM;
    g_root.inode->i_private = &g_root; g_block.inode->i_private = &g_block;
    g_root.inode->i_op = g_block.inode->i_op = &sys_iops; g_root.inode->i_fop = g_block.inode->i_fop = &sys_fops;
    for (i = 0; i < g_ndisks; i++) {
        block_device_t *disk = blk->disk_at(i);
        if (!disk) continue;
        memset(&g_disks[i], 0, sizeof(g_disks[i])); memset(&g_sizes[i], 0, sizeof(g_sizes[i]));
        strncpy(g_disks[i].name, disk->name, sizeof(g_disks[i].name) - 1);
        strcpy(g_sizes[i].name, "size"); g_disks[i].kind = SYS_DISK; g_sizes[i].kind = SYS_SIZE;
        g_disks[i].disk = g_sizes[i].disk = disk;
        g_disks[i].inode = alloc_inode(sb, S_IFDIR | 0555); g_sizes[i].inode = alloc_inode(sb, S_IFREG | 0444);
        if (!g_disks[i].inode || !g_sizes[i].inode) return -ENOMEM;
        g_disks[i].inode->i_private = &g_disks[i]; g_sizes[i].inode->i_private = &g_sizes[i];
        g_disks[i].inode->i_op = g_sizes[i].inode->i_op = &sys_iops; g_disks[i].inode->i_fop = g_sizes[i].inode->i_fop = &sys_fops;
    }
    rd = alloc_dentry("/", NULL, g_root.inode); if (!rd) return -ENOMEM;
    sb->s_root = rd; *sb_out = sb; return 0;
}

static file_system_type_t g_fs = {
    .name = "sysfs",
    .mount = sysfs_mount,
};

static int sysfs_init(driver_t *drv, void *ctx)
{
    const vfs_api_t *api = vfs_api_get();
    (void)drv; (void)ctx;
    if (!api || !api->register_filesystem || !api->alloc_inode || !api->alloc_dentry ||
        !api->mkdir || !api->mount)
        return -1;
    alloc_inode = (inode_t *(*)(super_block_t *, uint32_t))api->alloc_inode;
    alloc_dentry = (dentry_t *(*)(const char *, dentry_t *, inode_t *))api->alloc_dentry;
    if (api->register_filesystem(&g_fs) < 0)
        return -1;
    (void)api->mkdir("/sys", 0555);
    if (api->mount("sysfs", "/sys", "sysfs", 0, NULL) < 0)
        return -1;
    vga_print("sysfs: registered\n");
    return 0;
}

int kmod_init(void)
{
    driver_t d;
    memset(&d, 0, sizeof(d));
    strncpy(d.name, "sysfs", DRIVER_NAME_MAX - 1);
    strncpy(d.version, "0.1", DRIVER_VERSION_MAX - 1);
    d.kind = DRIVER_KIND_CUSTOM;
    d.class = DRIVER_CLASS_MISC;
    d.priority = 70;
    d.init = sysfs_init;
    if (driver_register(&d) < 0) return -1;
    if (driver_load("sysfs", NULL) < 0) return -1;
    return 0;
}
