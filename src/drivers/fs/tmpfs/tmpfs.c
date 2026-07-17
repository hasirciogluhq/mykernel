#include <kernel/vfs_api.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/string.h>
#include <drivers/vfs_fs.h>
#include <drivers/driver.h>
#include <drivers/vga.h>

#define TMPFS_DEFAULT_LIMIT (4u * 1024u * 1024u)
#define TMPFS_MAX_CHILDREN  128

typedef struct tmpfs_node {
    char name[64];
    uint32_t mode;
    uint64_t size;
    uint8_t *data;
    size_t capacity;
    struct tmpfs_node *parent;
    struct tmpfs_node *children[TMPFS_MAX_CHILDREN];
    size_t nchildren;
    inode_t *inode;
} tmpfs_node_t;

typedef struct tmpfs_sb {
    size_t limit;
    size_t used;
    tmpfs_node_t *root;
} tmpfs_sb_t;

static inode_t *(*alloc_inode)(super_block_t *, uint32_t);
static dentry_t *(*alloc_dentry)(const char *, dentry_t *, inode_t *);

static tmpfs_node_t *tmpfs_child(tmpfs_node_t *dir, const char *name)
{
    size_t i;
    for (i = 0; dir && i < dir->nchildren; i++)
        if (strcmp(dir->children[i]->name, name) == 0)
            return dir->children[i];
    return NULL;
}

static ssize_t tmpfs_read(file_t *file, void *buf, size_t count, off_t *pos)
{
    tmpfs_node_t *node = file ? file->f_inode->i_private : NULL;
    if (!node || S_ISDIR(node->mode))
        return -EISDIR;
    if (*pos < 0 || (uint64_t)*pos >= node->size)
        return 0;
    if (count > node->size - (uint64_t)*pos)
        count = (size_t)(node->size - (uint64_t)*pos);
    memcpy(buf, node->data + *pos, count);
    *pos += (off_t)count;
    return (ssize_t)count;
}

static ssize_t tmpfs_write(file_t *file, const void *buf, size_t count, off_t *pos)
{
    tmpfs_node_t *node = file ? file->f_inode->i_private : NULL;
    tmpfs_sb_t *fs;
    size_t end, newcap, added;
    uint8_t *data;

    if (!node || S_ISDIR(node->mode) || *pos < 0)
        return -EISDIR;
    fs = (tmpfs_sb_t *)file->f_inode->i_sb->s_fs_info;
    end = (size_t)*pos + count;
    if (end < (size_t)*pos)
        return -EOVERFLOW;
    added = end > node->size ? end - (size_t)node->size : 0;
    if (!fs || added > fs->limit - fs->used)
        return -ENOSPC;
    if (end > node->capacity) {
        newcap = node->capacity ? node->capacity : 256;
        while (newcap < end) {
            if (newcap > fs->limit / 2) {
                newcap = fs->limit;
                break;
            }
            newcap *= 2;
        }
        data = (uint8_t *)kmalloc(newcap);
        if (!data)
            return -ENOMEM;
        if (node->data && node->size)
            memcpy(data, node->data, (size_t)node->size);
        if (newcap > node->size)
            memset(data + node->size, 0, newcap - (size_t)node->size);
        node->data = data;
        node->capacity = newcap;
    }
    memcpy(node->data + *pos, buf, count);
    *pos += (off_t)count;
    if ((uint64_t)*pos > node->size) {
        fs->used += (size_t)*pos - (size_t)node->size;
        node->size = (uint64_t)*pos;
        node->inode->i_size = node->size;
    }
    return (ssize_t)count;
}

static int tmpfs_readdir(file_t *file, void *dirent, size_t max)
{
    tmpfs_node_t *dir = file ? file->f_inode->i_private : NULL;
    vfs_dirent_t *out = (vfs_dirent_t *)dirent;
    size_t i, written = 0;
    if (!dir || !S_ISDIR(dir->mode))
        return -ENOTDIR;
    for (i = (size_t)file->f_pos; i < dir->nchildren && written < max; i++) {
        tmpfs_node_t *n = dir->children[i];
        strncpy(out[written].name, n->name, sizeof(out[written].name) - 1);
        out[written].name[sizeof(out[written].name) - 1] = 0;
        out[written].ino = n->inode->i_ino;
        out[written].type = n->mode & S_IFMT;
        written++;
    }
    file->f_pos += (off_t)written;
    return (int)written;
}

static const file_operations_t tmpfs_fops = {
    .read = tmpfs_read, .write = tmpfs_write, .readdir = tmpfs_readdir
};

static dentry_t *tmpfs_lookup(inode_t *dir, dentry_t *dentry)
{
    tmpfs_node_t *node = tmpfs_child((tmpfs_node_t *)dir->i_private, dentry->d_name);
    return node ? alloc_dentry(dentry->d_name, dentry->d_parent, node->inode) : NULL;
}

static int tmpfs_create_node(inode_t *dir, dentry_t *dentry, int mode)
{
    tmpfs_node_t *parent = (tmpfs_node_t *)dir->i_private;
    tmpfs_node_t *node;
    inode_t *inode;
    if (!parent || !S_ISDIR(parent->mode))
        return -ENOTDIR;
    if (parent->nchildren >= TMPFS_MAX_CHILDREN)
        return -ENOSPC;
    if (tmpfs_child(parent, dentry->d_name))
        return -EEXIST;
    node = (tmpfs_node_t *)kmalloc(sizeof(*node));
    if (!node)
        return -ENOMEM;
    memset(node, 0, sizeof(*node));
    strncpy(node->name, dentry->d_name, sizeof(node->name) - 1);
    node->mode = (uint32_t)mode;
    if (!(node->mode & S_IFMT))
        node->mode |= S_IFREG;
    node->parent = parent;
    inode = alloc_inode(dir->i_sb, node->mode);
    if (!inode)
        return -ENOMEM;
    inode->i_private = node;
    inode->i_op = dir->i_op;
    inode->i_fop = &tmpfs_fops;
    node->inode = inode;
    parent->children[parent->nchildren++] = node;
    dentry->d_inode = inode;
    return 0;
}

static int tmpfs_create(inode_t *dir, dentry_t *dentry, int mode)
{
    return tmpfs_create_node(dir, dentry, (mode & 0777) | S_IFREG);
}

static int tmpfs_mkdir(inode_t *dir, dentry_t *dentry, int mode)
{
    return tmpfs_create_node(dir, dentry, (mode & 0777) | S_IFDIR);
}

static int tmpfs_remove(inode_t *dir, dentry_t *dentry, int want_dir)
{
    tmpfs_node_t *parent = (tmpfs_node_t *)dir->i_private;
    tmpfs_node_t *node;
    size_t i;
    if (!parent || !(node = tmpfs_child(parent, dentry->d_name)))
        return -ENOENT;
    if (want_dir) {
        if (!S_ISDIR(node->mode))
            return -ENOTDIR;
        if (node->nchildren)
            return -ENOTEMPTY;
    } else if (S_ISDIR(node->mode)) {
        return -EISDIR;
    }
    for (i = 0; i < parent->nchildren; i++)
        if (parent->children[i] == node)
            break;
    if (i == parent->nchildren)
        return -ENOENT;
    while (++i < parent->nchildren)
        parent->children[i - 1] = parent->children[i];
    parent->nchildren--;
    return 0;
}

static int tmpfs_unlink(inode_t *dir, dentry_t *dentry)
{
    return tmpfs_remove(dir, dentry, 0);
}

static int tmpfs_rmdir(inode_t *dir, dentry_t *dentry)
{
    return tmpfs_remove(dir, dentry, 1);
}

static int tmpfs_rename(inode_t *old_dir, dentry_t *old_d,
                        inode_t *new_dir, dentry_t *new_d)
{
    tmpfs_node_t *old_parent = (tmpfs_node_t *)old_dir->i_private;
    tmpfs_node_t *new_parent = (tmpfs_node_t *)new_dir->i_private;
    tmpfs_node_t *node;
    if (!old_parent || !new_parent || !(node = tmpfs_child(old_parent, old_d->d_name)))
        return -ENOENT;
    if (tmpfs_child(new_parent, new_d->d_name))
        return -EEXIST;
    if (new_parent->nchildren >= TMPFS_MAX_CHILDREN)
        return -ENOSPC;
    if (old_parent != new_parent) {
        size_t i;
        for (i = 0; i < old_parent->nchildren; i++)
            if (old_parent->children[i] == node)
                break;
        while (++i < old_parent->nchildren)
            old_parent->children[i - 1] = old_parent->children[i];
        old_parent->nchildren--;
        new_parent->children[new_parent->nchildren++] = node;
        node->parent = new_parent;
    }
    strncpy(node->name, new_d->d_name, sizeof(node->name) - 1);
    node->name[sizeof(node->name) - 1] = 0;
    return 0;
}

static const inode_operations_t tmpfs_iops = {
    .lookup = tmpfs_lookup, .create = tmpfs_create, .mkdir = tmpfs_mkdir,
    .unlink = tmpfs_unlink, .rmdir = tmpfs_rmdir, .rename = tmpfs_rename
};

static int tmpfs_mount(file_system_type_t *fs_type, int flags,
                       const char *dev_name, void *data, super_block_t **sb_out)
{
    super_block_t *sb;
    tmpfs_sb_t *fs;
    inode_t *root_inode;
    dentry_t *root_dentry;
    size_t limit = data ? *(const size_t *)data : TMPFS_DEFAULT_LIMIT;
    (void)flags; (void)dev_name;
    if (!sb_out || !limit)
        return -EINVAL;
    sb = (super_block_t *)kmalloc(sizeof(*sb));
    fs = (tmpfs_sb_t *)kmalloc(sizeof(*fs));
    fs->root = (tmpfs_node_t *)kmalloc(sizeof(*fs->root));
    if (!sb || !fs || !fs->root)
        return -ENOMEM;
    memset(sb, 0, sizeof(*sb));
    memset(fs, 0, sizeof(*fs));
    memset(fs->root, 0, sizeof(*fs->root));
    fs->limit = limit;
    fs->root->mode = S_IFDIR | 0755;
    strcpy(fs->root->name, "/");
    root_inode = alloc_inode(sb, fs->root->mode);
    if (!root_inode)
        return -ENOMEM;
    root_inode->i_op = &tmpfs_iops;
    root_inode->i_fop = &tmpfs_fops;
    root_inode->i_private = fs->root;
    fs->root->inode = root_inode;
    root_dentry = alloc_dentry("/", NULL, root_inode);
    if (!root_dentry)
        return -ENOMEM;
    sb->s_type = fs_type;
    sb->s_root = root_dentry;
    sb->s_fs_info = fs;
    *sb_out = sb;
    return 0;
}

static file_system_type_t g_fs = {
    .name = "tmpfs",
    .mount = tmpfs_mount,
};

static int tmpfs_init(driver_t *drv, void *ctx)
{
    const vfs_api_t *api = vfs_api_get();
    (void)drv; (void)ctx;
    if (!api || !api->register_filesystem || !api->alloc_inode || !api->alloc_dentry)
        return -1;
    alloc_inode = (inode_t *(*)(super_block_t *, uint32_t))api->alloc_inode;
    alloc_dentry = (dentry_t *(*)(const char *, dentry_t *, inode_t *))api->alloc_dentry;
    if (api->register_filesystem(&g_fs) < 0)
        return -1;
    vga_print("tmpfs: registered\n");
    return 0;
}

int kmod_init(void)
{
    driver_t d;
    memset(&d, 0, sizeof(d));
    strncpy(d.name, "tmpfs", DRIVER_NAME_MAX - 1);
    strncpy(d.version, "0.1", DRIVER_VERSION_MAX - 1);
    d.kind = DRIVER_KIND_CUSTOM;
    d.class = DRIVER_CLASS_MISC;
    d.priority = 70;
    d.init = tmpfs_init;
    if (driver_register(&d) < 0) return -1;
    if (driver_load("tmpfs", NULL) < 0) return -1;
    return 0;
}
