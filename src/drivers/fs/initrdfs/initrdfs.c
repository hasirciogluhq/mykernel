#include <kernel/vfs_api.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/initrd.h>
#include <kernel/string.h>
#include <drivers/vfs_fs.h>
#include <drivers/driver.h>
#include <drivers/vga.h>

extern void *ksym_lookup(const char *name);
typedef const void *(*initrd_get_t)(size_t *);
typedef struct initrd_node { const initrd_header_t *header; uint32_t index; inode_t *inode; } initrd_node_t;
static inode_t *(*alloc_inode)(super_block_t *, uint32_t);
static dentry_t *(*alloc_dentry)(const char *, dentry_t *, inode_t *);
static initrd_get_t initrd_get;
static initrd_node_t g_root, g_nodes[INITRD_MAX_FILES];

static ssize_t initrd_read(file_t *file, void *buf, size_t count, off_t *pos)
{
    initrd_node_t *n = file->f_inode->i_private; const initrd_file_t *f;
    if (!n || n->index >= n->header->count) return -EISDIR;
    f = &n->header->files[n->index];
    if (*pos < 0 || (uint64_t)*pos >= f->size) return 0;
    if (count > f->size - (size_t)*pos) count = f->size - (size_t)*pos;
    memcpy(buf, (const uint8_t *)n->header + f->offset + *pos, count);
    *pos += (off_t)count; return (ssize_t)count;
}
static dentry_t *initrd_lookup(inode_t *dir, dentry_t *dentry)
{
    initrd_node_t *root = dir->i_private; uint32_t i;
    if (root != &g_root) return NULL;
    for (i = 0; i < root->header->count; i++)
        if (strcmp(root->header->files[i].name, dentry->d_name) == 0)
            return alloc_dentry(dentry->d_name, dentry->d_parent, g_nodes[i].inode);
    return NULL;
}
static int initrd_readdir(file_t *file, void *dirent, size_t max)
{
    initrd_node_t *root = file->f_inode->i_private; vfs_dirent_t *out = dirent; size_t i, n = 0;
    if (root != &g_root) return -ENOTDIR;
    for (i = (size_t)file->f_pos; i < root->header->count && n < max; i++, n++) {
        strncpy(out[n].name, root->header->files[i].name, sizeof(out[n].name) - 1);
        out[n].ino = g_nodes[i].inode->i_ino; out[n].type = S_IFREG;
    }
    file->f_pos += (off_t)n; return (int)n;
}
static const inode_operations_t initrd_iops = { .lookup = initrd_lookup };
static const file_operations_t initrd_fops = { .read = initrd_read, .readdir = initrd_readdir };

static int initrdfs_mount(file_system_type_t *fs_type, int flags,
                          const char *dev_name, void *data, super_block_t **sb_out)
{
    const initrd_header_t *header; size_t size = 0; super_block_t *sb; inode_t *root; dentry_t *rd; uint32_t i;
    (void)flags; (void)dev_name; (void)data;
    if (!initrd_get || !(header = initrd_get(&size)) || size < sizeof(*header) ||
        header->magic != INITRD_MAGIC || header->count > INITRD_MAX_FILES) return -EINVAL;
    for (i = 0; i < header->count; i++)
        if (header->files[i].offset > size || header->files[i].size > size - header->files[i].offset) return -EINVAL;
    sb = kmalloc(sizeof(*sb)); if (!sb) return -ENOMEM; memset(sb, 0, sizeof(*sb)); sb->s_type = fs_type;
    g_root.header = header; g_root.index = INITRD_MAX_FILES;
    root = alloc_inode(sb, S_IFDIR | 0555); if (!root) return -ENOMEM;
    root->i_private = &g_root; root->i_op = &initrd_iops; root->i_fop = &initrd_fops;
    for (i = 0; i < header->count; i++) {
        g_nodes[i].header = header; g_nodes[i].index = i; g_nodes[i].inode = alloc_inode(sb, S_IFREG | 0444);
        if (!g_nodes[i].inode) return -ENOMEM;
        g_nodes[i].inode->i_size = header->files[i].size; g_nodes[i].inode->i_private = &g_nodes[i];
        g_nodes[i].inode->i_op = &initrd_iops; g_nodes[i].inode->i_fop = &initrd_fops;
    }
    rd = alloc_dentry("/", NULL, root); if (!rd) return -ENOMEM; sb->s_root = rd; sb->s_fs_info = (void *)header; *sb_out = sb; return 0;
}

static file_system_type_t g_fs = {
    .name = "initrdfs",
    .mount = initrdfs_mount,
};

static int initrdfs_init(driver_t *drv, void *ctx)
{
    const vfs_api_t *api = vfs_api_get();
    (void)drv; (void)ctx;
    if (!api || !api->register_filesystem || !api->alloc_inode || !api->alloc_dentry)
        return -1;
    alloc_inode = (inode_t *(*)(super_block_t *, uint32_t))api->alloc_inode;
    alloc_dentry = (dentry_t *(*)(const char *, dentry_t *, inode_t *))api->alloc_dentry;
    initrd_get = (initrd_get_t)ksym_lookup("initrd_store_get");
    if (!initrd_get) return -ENODEV;
    if (api->register_filesystem(&g_fs) < 0)
        return -1;
    vga_print("initrdfs: registered\n");
    return 0;
}

int kmod_init(void)
{
    driver_t d;
    memset(&d, 0, sizeof(d));
    strncpy(d.name, "initrdfs", DRIVER_NAME_MAX - 1);
    strncpy(d.version, "0.1", DRIVER_VERSION_MAX - 1);
    d.kind = DRIVER_KIND_CUSTOM;
    d.class = DRIVER_CLASS_MISC;
    d.priority = 70;
    d.init = initrdfs_init;
    if (driver_register(&d) < 0) return -1;
    if (driver_load("initrdfs", NULL) < 0) return -1;
    return 0;
}
