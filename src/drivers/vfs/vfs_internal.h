#ifndef MYKERNEL_VFS_INTERNAL_H
#define MYKERNEL_VFS_INTERNAL_H

#include <drivers/vfs_fs.h>

void vfs_dcache_init(void);
dentry_t *vfs_dcache_lookup(dentry_t *parent, const char *name);
void vfs_dcache_add(dentry_t *d);
void vfs_dcache_remove(dentry_t *d);
uint32_t vfs_d_hash(const char *name, size_t len);

void vfs_pcache_init(void);
ssize_t vfs_cached_read(inode_t *inode, void *buf, size_t count, off_t pos);
ssize_t vfs_cached_write(inode_t *inode, const void *buf, size_t count, off_t pos);
int vfs_cache_invalidate(inode_t *inode);

int vfs_xattr_get(inode_t *inode, const char *name, void *value, size_t size);
int vfs_xattr_set(inode_t *inode, const char *name, const void *value, size_t size, int flags);
int vfs_xattr_list(inode_t *inode, char *list, size_t size);
int vfs_xattr_remove(inode_t *inode, const char *name);

void vfs_flock_init(void);
int vfs_flock(file_t *file, int cmd, flock_t *fl);

void vfs_fsnotify_init(void);
int vfs_fsnotify_add_watch(const char *path, uint32_t mask);
int vfs_fsnotify_rm_watch(int wd);
void vfs_fsnotify_event(const char *path, uint32_t mask, const char *name);
int vfs_fsnotify_read(int wd, fsnotify_event_t *events, size_t max);

#endif
