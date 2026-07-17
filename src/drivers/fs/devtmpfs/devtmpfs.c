#include <kernel/vfs_api.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/string.h>
#include <drivers/vfs_fs.h>
#include <drivers/driver.h>
#include <drivers/vga.h>
#include <drivers/console.h>

extern void *ksym_lookup(const char *name);

static ssize_t (*console_read_fn)(void *, size_t);
static void (*console_write_fn)(const char *, size_t);
static dentry_t *(*alloc_dentry)(const char *, dentry_t *, inode_t *);
static inode_t *(*alloc_inode)(super_block_t *, uint32_t);

static ssize_t cons_read(file_t *f, void *buf, size_t count, off_t *pos)
{
    (void)f;
    (void)pos;
    if (!console_read_fn)
        return -EIO;
    return console_read_fn(buf, count);
}

static ssize_t cons_write(file_t *f, const void *buf, size_t count, off_t *pos)
{
    (void)f;
    (void)pos;
    if (!console_write_fn)
        return -EIO;
    console_write_fn((const char *)buf, count);
    return (ssize_t)count;
}

static ssize_t null_read(file_t *f, void *buf, size_t count, off_t *pos)
{
    (void)f;
    (void)buf;
    (void)count;
    (void)pos;
    return 0;
}

static ssize_t null_write(file_t *f, const void *buf, size_t count, off_t *pos)
{
    (void)f;
    (void)buf;
    (void)pos;
    return (ssize_t)count;
}

static ssize_t zero_read(file_t *f, void *buf, size_t count, off_t *pos)
{
    (void)f;
    (void)pos;
    memset(buf, 0, count);
    return (ssize_t)count;
}

typedef struct dev_node {
    char name[32];
    const file_operations_t *fops;
    inode_t *inode;
} dev_node_t;

#define DEV_MAX 16
static dev_node_t g_devs[DEV_MAX];
static int g_ndev;
static inode_t *g_root_ino;
static super_block_t *g_sb;

static const file_operations_t cons_fops = { .read = cons_read, .write = cons_write };
static const file_operations_t null_fops = { .read = null_read, .write = null_write };
static const file_operations_t zero_fops = { .read = zero_read, .write = null_write };

static int add_dev(const char *name, const file_operations_t *fops)
{
    inode_t *ino;
    if (g_ndev >= DEV_MAX)
        return -ENOSPC;
    ino = alloc_inode(g_sb, S_IFCHR | 0666);
    if (!ino)
        return -ENOMEM;
    ino->i_fop = fops;
    strncpy(g_devs[g_ndev].name, name, sizeof(g_devs[g_ndev].name) - 1);
    g_devs[g_ndev].fops = fops;
    g_devs[g_ndev].inode = ino;
    g_ndev++;
    return 0;
}

static dentry_t *dev_lookup(inode_t *dir, dentry_t *dentry)
{
    int i;
    (void)dir;
    for (i = 0; i < g_ndev; i++) {
        if (strcmp(g_devs[i].name, dentry->d_name) == 0)
            return alloc_dentry(dentry->d_name, dentry->d_parent, g_devs[i].inode);
    }
    return NULL;
}

static int dev_readdir(file_t *file, void *dirent, size_t max)
{
    vfs_dirent_t *out = (vfs_dirent_t *)dirent;
    size_t i, w = 0;
    (void)file;
    for (i = 0; i < (size_t)g_ndev && w < max; i++) {
        strncpy(out[w].name, g_devs[i].name, sizeof(out[w].name) - 1);
        out[w].type = S_IFCHR;
        out[w].ino = g_devs[i].inode->i_ino;
        w++;
    }
    return (int)w;
}

static const inode_operations_t dev_iops = { .lookup = dev_lookup };
static const file_operations_t dev_dir_fops = { .readdir = dev_readdir };

static int devtmpfs_mount(file_system_type_t *fs_type, int flags,
                          const char *dev_name, void *data, super_block_t **sb_out)
{
    dentry_t *rd;
    (void)flags;
    (void)dev_name;
    (void)data;

    g_sb = (super_block_t *)kmalloc(sizeof(*g_sb));
    if (!g_sb)
        return -ENOMEM;
    memset(g_sb, 0, sizeof(*g_sb));
    g_sb->s_type = fs_type;

    g_root_ino = alloc_inode(g_sb, S_IFDIR | 0755);
    if (!g_root_ino)
        return -ENOMEM;
    g_root_ino->i_op = &dev_iops;
    g_root_ino->i_fop = &dev_dir_fops;

    rd = alloc_dentry("/", NULL, g_root_ino);
    if (!rd)
        return -ENOMEM;
    g_sb->s_root = rd;

    g_ndev = 0;
    add_dev("console", &cons_fops);
    add_dev("null", &null_fops);
    add_dev("zero", &zero_fops);

    *sb_out = g_sb;
    return 0;
}

static file_system_type_t g_devtmpfs = {
    .name = "devtmpfs",
    .mount = devtmpfs_mount,
};

static int devtmpfs_init(driver_t *drv, void *ctx)
{
    const vfs_api_t *api = vfs_api_get();
    (void)drv;
    (void)ctx;
    if (!api || !api->register_filesystem || !api->mount || !api->mkdir)
        return -1;

    if (!api->alloc_inode || !api->alloc_dentry)
        return -1;
    alloc_inode = (inode_t *(*)(super_block_t *, uint32_t))api->alloc_inode;
    alloc_dentry = (dentry_t *(*)(const char *, dentry_t *, inode_t *))api->alloc_dentry;
    console_read_fn = (ssize_t (*)(void *, size_t))ksym_lookup("console_read");
    console_write_fn = (void (*)(const char *, size_t))ksym_lookup("console_write");

    if (api->register_filesystem(&g_devtmpfs) < 0)
        return -1;
    api->mkdir("/dev", 0755);
    if (api->mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) < 0)
        return -1;
    vga_print("devtmpfs: /dev ready\n");
    return 0;
}

int kmod_init(void)
{
    driver_t d;
    memset(&d, 0, sizeof(d));
    strncpy(d.name, "devtmpfs", DRIVER_NAME_MAX - 1);
    strncpy(d.version, "1.0", DRIVER_VERSION_MAX - 1);
    d.kind = DRIVER_KIND_CUSTOM;
    d.class = DRIVER_CLASS_MISC;
    d.priority = 40;
    d.init = devtmpfs_init;
    if (driver_register(&d) < 0)
        return -1;
    if (driver_load("devtmpfs", NULL) < 0)
        return -1;
    return 0;
}
