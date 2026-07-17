#include <kernel/vfs_api.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/string.h>
#include <drivers/vfs_fs.h>
#include <drivers/driver.h>
#include <drivers/vga.h>

#define RAMFS_MAX_CHILDREN 64
#define RAMFS_MAX_NAME     64

typedef struct ramfs_node {
    char                 name[RAMFS_MAX_NAME];
    uint32_t             mode;
    uint64_t             size;
    uint8_t             *data;
    size_t               capacity;
    struct ramfs_node   *parent;
    struct ramfs_node   *children[RAMFS_MAX_CHILDREN];
    int                  nchildren;
    inode_t             *inode;
} ramfs_node_t;

static dentry_t *(*alloc_dentry)(const char *, dentry_t *, inode_t *);
static inode_t  *(*alloc_inode)(super_block_t *, uint32_t);

static ssize_t ram_read(file_t *file, void *buf, size_t count, off_t *pos)
{
    ramfs_node_t *n = (ramfs_node_t *)file->f_inode->i_private;
    size_t avail;
    if (!n || !n->data)
        return 0;
    if ((uint64_t)*pos >= n->size)
        return 0;
    avail = (size_t)(n->size - (uint64_t)*pos);
    if (count > avail)
        count = avail;
    memcpy(buf, n->data + *pos, count);
    *pos += (off_t)count;
    return (ssize_t)count;
}

static ssize_t ram_write(file_t *file, const void *buf, size_t count, off_t *pos)
{
    ramfs_node_t *n = (ramfs_node_t *)file->f_inode->i_private;
    size_t need;
    uint8_t *nd;
    if (!n)
        return -EIO;
    need = (size_t)(*pos) + count;
    if (need > n->capacity) {
        size_t cap = n->capacity ? n->capacity * 2 : 256;
        while (cap < need)
            cap *= 2;
        nd = (uint8_t *)kmalloc(cap);
        if (!nd)
            return -ENOMEM;
        if (n->data && n->size)
            memcpy(nd, n->data, (size_t)n->size);
        memset(nd + n->size, 0, cap - (size_t)n->size);
        n->data = nd;
        n->capacity = cap;
    }
    memcpy(n->data + *pos, buf, count);
    *pos += (off_t)count;
    if ((uint64_t)*pos > n->size)
        n->size = (uint64_t)*pos;
    file->f_inode->i_size = n->size;
    return (ssize_t)count;
}

static int ram_readdir(file_t *file, void *dirent, size_t max)
{
    ramfs_node_t *n = (ramfs_node_t *)file->f_inode->i_private;
    vfs_dirent_t *out = (vfs_dirent_t *)dirent;
    size_t i, written = 0;
    if (!n || !S_ISDIR(n->mode))
        return -ENOTDIR;
    for (i = 0; i < (size_t)n->nchildren && written < max; i++) {
        strncpy(out[written].name, n->children[i]->name, sizeof(out[written].name) - 1);
        out[written].type = n->children[i]->mode & S_IFMT;
        out[written].ino = n->children[i]->inode ? n->children[i]->inode->i_ino : 0;
        written++;
    }
    return (int)written;
}

static const file_operations_t ram_fops = {
    .read = ram_read,
    .write = ram_write,
    .readdir = ram_readdir,
};

static ramfs_node_t *find_child(ramfs_node_t *dir, const char *name)
{
    int i;
    for (i = 0; i < dir->nchildren; i++) {
        if (strcmp(dir->children[i]->name, name) == 0)
            return dir->children[i];
    }
    return NULL;
}

static dentry_t *ram_lookup(inode_t *dir, dentry_t *dentry)
{
    ramfs_node_t *dn = (ramfs_node_t *)dir->i_private;
    ramfs_node_t *child;
    dentry_t *out;
    if (!dn || !dentry)
        return NULL;
    child = find_child(dn, dentry->d_name);
    if (!child || !child->inode)
        return NULL;
    out = alloc_dentry(dentry->d_name, dentry->d_parent, child->inode);
    return out;
}

static int ram_create(inode_t *dir, dentry_t *dentry, int mode)
{
    ramfs_node_t *dn = (ramfs_node_t *)dir->i_private;
    ramfs_node_t *n;
    inode_t *ino;
    if (!dn || dn->nchildren >= RAMFS_MAX_CHILDREN)
        return -ENOSPC;
    if (find_child(dn, dentry->d_name))
        return -EEXIST;
    n = (ramfs_node_t *)kmalloc(sizeof(*n));
    if (!n)
        return -ENOMEM;
    memset(n, 0, sizeof(*n));
    strncpy(n->name, dentry->d_name, sizeof(n->name) - 1);
    n->mode = (uint32_t)(mode ? mode : S_IFREG);
    if (!(n->mode & S_IFMT))
        n->mode |= S_IFREG;
    n->parent = dn;
    ino = alloc_inode(dir->i_sb, n->mode);
    if (!ino)
        return -ENOMEM;
    ino->i_private = n;
    ino->i_fop = &ram_fops;
    ino->i_op = dir->i_op;
    n->inode = ino;
    dn->children[dn->nchildren++] = n;
    dentry->d_inode = ino;
    return 0;
}

static int ram_mkdir(inode_t *dir, dentry_t *dentry, int mode)
{
    return ram_create(dir, dentry, (mode & 0777) | S_IFDIR);
}

static const inode_operations_t ram_iops = {
    .lookup = ram_lookup,
    .create = ram_create,
    .mkdir = ram_mkdir,
};

static int ramfs_mount(file_system_type_t *fs_type, int flags,
                       const char *dev_name, void *data, super_block_t **sb_out)
{
    super_block_t *sb;
    ramfs_node_t *root;
    inode_t *rino;
    dentry_t *rd;
    (void)fs_type;
    (void)flags;
    (void)dev_name;
    (void)data;

    sb = (super_block_t *)kmalloc(sizeof(*sb));
    root = (ramfs_node_t *)kmalloc(sizeof(*root));
    if (!sb || !root)
        return -ENOMEM;
    memset(sb, 0, sizeof(*sb));
    memset(root, 0, sizeof(*root));
    strcpy(root->name, "/");
    root->mode = S_IFDIR | 0755;

    rino = alloc_inode(sb, root->mode);
    if (!rino)
        return -ENOMEM;
    rino->i_op = &ram_iops;
    rino->i_fop = &ram_fops;
    rino->i_private = root;
    root->inode = rino;

    rd = alloc_dentry("/", NULL, rino);
    if (!rd)
        return -ENOMEM;

    sb->s_type = fs_type;
    sb->s_root = rd;
    sb->s_fs_info = root;
    *sb_out = sb;
    return 0;
}

static file_system_type_t g_ramfs = {
    .name = "ramfs",
    .mount = ramfs_mount,
    .kill_sb = NULL,
};

static int ramfs_init(driver_t *drv, void *ctx)
{
    const vfs_api_t *api = vfs_api_get();
    (void)drv;
    (void)ctx;
    if (!api || !api->register_filesystem || !api->mount)
        return -1;
    if (!api->alloc_inode || !api->alloc_dentry)
        return -1;
    alloc_inode = (inode_t *(*)(super_block_t *, uint32_t))api->alloc_inode;
    alloc_dentry = (dentry_t *(*)(const char *, dentry_t *, inode_t *))api->alloc_dentry;
    if (api->register_filesystem(&g_ramfs) < 0)
        return -1;
    /* Auto-mount as root if none yet */
    if (api->mount(NULL, "/", "ramfs", 0, NULL) < 0)
        return -1;
    if (api->mkdir) {
        (void)api->mkdir("/root", 0755);
        (void)api->mkdir("/home", 0755);
        (void)api->mkdir("/tmp", 0755);
        (void)api->mkdir("/etc", 0755);
        (void)api->mkdir("/usr", 0755);
        (void)api->mkdir("/var", 0755);
    }
    vga_print("ramfs: mounted /\n");
    return 0;
}

int kmod_init(void)
{
    driver_t d;
    memset(&d, 0, sizeof(d));
    strncpy(d.name, "ramfs", DRIVER_NAME_MAX - 1);
    strncpy(d.version, "1.0", DRIVER_VERSION_MAX - 1);
    d.kind = DRIVER_KIND_CUSTOM;
    d.class = DRIVER_CLASS_MISC;
    d.priority = 30;
    d.init = ramfs_init;
    if (driver_register(&d) < 0)
        return -1;
    if (driver_load("ramfs", NULL) < 0)
        return -1;
    return 0;
}
