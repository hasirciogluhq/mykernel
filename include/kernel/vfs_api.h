#ifndef MYKERNEL_KERNEL_VFS_API_H
#define MYKERNEL_KERNEL_VFS_API_H

#include <kernel/types.h>

/*
 * Thin ABI: core syscalls → vfs.kmod.
 * Unsupported / missing ops return -ENOTSUP (hard fail).
 */
typedef struct vfs_aio {
    int      fd;
    void    *buf;
    size_t   count;
    off_t    pos;
    int      write; /* 0=read 1=write */
    volatile int done;
    ssize_t  result;
    void   (*complete)(struct vfs_aio *aio);
    void    *private_data;
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

    /* Kernel-level async I/O (completion may run from block poll). */
    int     (*aio_submit)(vfs_aio_t *aio);
    int     (*aio_wait)(vfs_aio_t *aio);
    int     (*aio_poll)(void);

    /* Driver registration surface (used by other kmods via ksym). */
    int     (*register_filesystem)(const void *fs_type);
    int     (*unregister_filesystem)(const char *name);

    /* Alloc helpers for filesystem kmods (typed as void* to keep ABI thin). */
    void   *(*alloc_inode)(void *sb, uint32_t mode);
    void   *(*alloc_dentry)(const char *name, void *parent, void *inode);
} vfs_api_t;

void              vfs_api_register(const vfs_api_t *api);
const vfs_api_t  *vfs_api_get(void);

#endif
