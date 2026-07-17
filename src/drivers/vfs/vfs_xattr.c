#include <drivers/vfs_fs.h>
#include <kernel/heap.h>
#include <kernel/string.h>
#include <kernel/errno.h>

int vfs_xattr_get(inode_t *inode, const char *name, void *value, size_t size)
{
    xattr_entry_t *e;
    if (!inode || !name)
        return -EINVAL;
    if (inode->i_op && inode->i_op->getxattr) {
        dentry_t fake;
        memset(&fake, 0, sizeof(fake));
        fake.d_inode = inode;
        return inode->i_op->getxattr(&fake, name, value, size);
    }
    for (e = inode->i_xattrs; e; e = e->next) {
        if (strcmp(e->name, name) == 0) {
            if (!value)
                return (int)e->size;
            if (size < e->size)
                return -ERANGE;
            memcpy(value, e->value, e->size);
            return (int)e->size;
        }
    }
    return -ENOENT;
}

int vfs_xattr_set(inode_t *inode, const char *name, const void *value, size_t size, int flags)
{
    xattr_entry_t *e, **pp;
    void *nv;
    (void)flags;
    if (!inode || !name)
        return -EINVAL;
    if (inode->i_op && inode->i_op->setxattr) {
        dentry_t fake;
        memset(&fake, 0, sizeof(fake));
        fake.d_inode = inode;
        return inode->i_op->setxattr(&fake, name, value, size, flags);
    }
    for (pp = &inode->i_xattrs; *pp; pp = &(*pp)->next) {
        if (strcmp((*pp)->name, name) == 0) {
            nv = kmalloc(size ? size : 1);
            if (!nv)
                return -ENOMEM;
            if (size)
                memcpy(nv, value, size);
            e = *pp;
            e->value = nv;
            e->size = size;
            return 0;
        }
    }
    e = (xattr_entry_t *)kmalloc(sizeof(*e));
    if (!e)
        return -ENOMEM;
    memset(e, 0, sizeof(*e));
    strncpy(e->name, name, sizeof(e->name) - 1);
    e->value = kmalloc(size ? size : 1);
    if (!e->value)
        return -ENOMEM;
    if (size)
        memcpy(e->value, value, size);
    e->size = size;
    e->next = inode->i_xattrs;
    inode->i_xattrs = e;
    return 0;
}

int vfs_xattr_list(inode_t *inode, char *list, size_t size)
{
    xattr_entry_t *e;
    size_t need = 0;
    if (!inode)
        return -EINVAL;
    if (inode->i_op && inode->i_op->listxattr) {
        dentry_t fake;
        memset(&fake, 0, sizeof(fake));
        fake.d_inode = inode;
        return inode->i_op->listxattr(&fake, list, size);
    }
    for (e = inode->i_xattrs; e; e = e->next)
        need += strlen(e->name) + 1;
    if (!list)
        return (int)need;
    if (size < need)
        return -ERANGE;
    need = 0;
    for (e = inode->i_xattrs; e; e = e->next) {
        size_t n = strlen(e->name) + 1;
        memcpy(list + need, e->name, n);
        need += n;
    }
    return (int)need;
}

int vfs_xattr_remove(inode_t *inode, const char *name)
{
    xattr_entry_t **pp;
    if (!inode || !name)
        return -EINVAL;
    if (inode->i_op && inode->i_op->removexattr) {
        dentry_t fake;
        memset(&fake, 0, sizeof(fake));
        fake.d_inode = inode;
        return inode->i_op->removexattr(&fake, name);
    }
    for (pp = &inode->i_xattrs; *pp; pp = &(*pp)->next) {
        if (strcmp((*pp)->name, name) == 0) {
            xattr_entry_t *e = *pp;
            *pp = e->next;
            return 0;
        }
    }
    return -ENOENT;
}
