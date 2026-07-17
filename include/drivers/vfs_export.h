#ifndef MYKERNEL_DRIVERS_VFS_EXPORT_H
#define MYKERNEL_DRIVERS_VFS_EXPORT_H

#include <drivers/vfs_fs.h>

/* Exported from vfs.kmod via ksym for other filesystem drivers. */
inode_t  *vfs_alloc_inode(super_block_t *sb, uint32_t mode);
dentry_t *vfs_alloc_dentry(const char *name, dentry_t *parent, inode_t *inode);

#endif
