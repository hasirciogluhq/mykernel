#ifndef MYKERNEL_KERNEL_VFS_API_H
#define MYKERNEL_KERNEL_VFS_API_H

#include <kernel/types.h>

typedef struct vfs_aio {
    int      fd;
    void    *buf;
    size_t   count;
    off_t    pos;
    int      write;
    volatile int done;
    ssize_t  result;
    void   (*complete)(struct vfs_aio *aio);
    void    *private_data;
    int      waiter_tid; /* blocked thread tid, or -1 */
} vfs_aio_t;

typedef struct vfs_api {
    int     (*open)(const char *path, int flags);
    ssize_t (*read)(int fd, void *buf, size_t count);
    ssize_t (*write)(int fd, const void *buf, size_t count);
    int     (*close)(int fd);
    off_t   (*lseek)(int fd, off_t off, int whence);
    int     (*mkdir)(const char *path, int mode);
    int     (*rmdir)(const char *path);
    int     (*unlink)(const char *path);
    int     (*rename)(const char *oldpath, const char *newpath);
    int     (*stat)(const char *path, void *statbuf);
    int     (*fstat)(int fd, void *statbuf);
    int     (*mount)(const char *source, const char *target,
                     const char *fstype, unsigned long flags, const void *data);
    int     (*umount)(const char *target, int flags);
    int     (*ioctl)(int fd, unsigned long cmd, void *arg);
    int     (*fsync)(int fd);
    int     (*readdir)(int fd, void *dirent, size_t max);

    int     (*aio_submit)(vfs_aio_t *aio);
    int     (*aio_wait)(vfs_aio_t *aio);
    int     (*aio_poll)(void);

    int     (*register_filesystem)(const void *fs_type);
    int     (*unregister_filesystem)(const char *name);
    void   *(*alloc_inode)(void *sb, uint32_t mode);
    void   *(*alloc_dentry)(const char *name, void *parent, void *inode);

    /* xattr */
    int (*getxattr)(const char *path, const char *name, void *value, size_t size);
    int (*setxattr)(const char *path, const char *name, const void *value, size_t size, int flags);
    int (*listxattr)(const char *path, char *list, size_t size);
    int (*removexattr)(const char *path, const char *name);

    /* flock */
    int (*flock)(int fd, int cmd, void *fl);

    /* fsnotify */
    int (*fsnotify_add_watch)(const char *path, uint32_t mask);
    int (*fsnotify_rm_watch)(int wd);
    int (*fsnotify_read)(int wd, void *events, size_t max);

    /* page cache helpers for FS drivers */
    ssize_t (*cached_read)(void *inode, void *buf, size_t count, off_t pos);
    ssize_t (*cached_write)(void *inode, const void *buf, size_t count, off_t pos);
    int     (*cache_invalidate)(void *inode);

    /* load kmod from filesystem path */
    int (*module_load_path)(const char *path);
} vfs_api_t;

void              vfs_api_register(const vfs_api_t *api);
const vfs_api_t  *vfs_api_get(void);

#endif
