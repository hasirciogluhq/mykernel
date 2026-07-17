#ifndef MYKERNEL_VFS_H
#define MYKERNEL_VFS_H

#include <kernel/types.h>
#include <kernel/vfs_api.h>
#include <kernel/errno.h>

#define VFS_MAX_FD      16
#define VFS_PATH_MAX    256

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x40
#define O_TRUNC  0x200
#define O_APPEND 0x400
#define O_DIRECTORY 0x10000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define MS_RDONLY 1
#define MS_BIND   0x1000
#define MNT_FORCE 1

/* Core bootstrap / thin shim → vfs.kmod */
void    vfs_init(void);
int     vfs_open(const char *path, int flags);
ssize_t vfs_read(int fd, void *buf, size_t count);
ssize_t vfs_write(int fd, const void *buf, size_t count);
int     vfs_close(int fd);
off_t   vfs_lseek(int fd, off_t off, int whence);
int     vfs_mount(const char *source, const char *target,
                  const char *fstype, unsigned long flags, const void *data);
int     vfs_umount(const char *target, int flags);
int     vfs_mkdir(const char *path, int mode);
int     vfs_aio_submit(vfs_aio_t *aio);
int     vfs_aio_wait(vfs_aio_t *aio);

#endif
