#include <kernel/vfs.h>
#include <kernel/errno.h>

void vfs_init(void)
{
    /* Real VFS lives in vfs.kmod and registers via vfs_api_register. */
}

static const vfs_api_t *api(void)
{
    return vfs_api_get();
}

int vfs_open(const char *path, int flags)
{
    const vfs_api_t *a = api();
    if (!a || !a->open)
        return -ENOTSUP;
    return a->open(path, flags);
}

ssize_t vfs_read(int fd, void *buf, size_t count)
{
    const vfs_api_t *a = api();
    if (!a || !a->read)
        return -ENOTSUP;
    return a->read(fd, buf, count);
}

ssize_t vfs_write(int fd, const void *buf, size_t count)
{
    const vfs_api_t *a = api();
    if (!a || !a->write)
        return -ENOTSUP;
    return a->write(fd, buf, count);
}

int vfs_close(int fd)
{
    const vfs_api_t *a = api();
    if (!a || !a->close)
        return -ENOTSUP;
    return a->close(fd);
}

off_t vfs_lseek(int fd, off_t off, int whence)
{
    const vfs_api_t *a = api();
    if (!a || !a->lseek)
        return -ENOTSUP;
    return a->lseek(fd, off, whence);
}

int vfs_mount(const char *source, const char *target,
              const char *fstype, unsigned long flags, const void *data)
{
    const vfs_api_t *a = api();
    if (!a || !a->mount)
        return -ENOTSUP;
    return a->mount(source, target, fstype, flags, data);
}

int vfs_umount(const char *target, int flags)
{
    const vfs_api_t *a = api();
    if (!a || !a->umount)
        return -ENOTSUP;
    return a->umount(target, flags);
}

int vfs_mkdir(const char *path, int mode)
{
    const vfs_api_t *a = api();
    if (!a || !a->mkdir)
        return -ENOTSUP;
    return a->mkdir(path, mode);
}

int vfs_readdir(int fd, void *dirent, size_t max)
{
    const vfs_api_t *a = api();
    if (!a || !a->readdir)
        return -ENOTSUP;
    return a->readdir(fd, dirent, max);
}

int vfs_aio_submit(vfs_aio_t *aio)
{
    const vfs_api_t *a = api();
    if (!a || !a->aio_submit)
        return -ENOTSUP;
    return a->aio_submit(aio);
}

int vfs_aio_wait(vfs_aio_t *aio)
{
    const vfs_api_t *a = api();
    if (!a || !a->aio_wait)
        return -ENOTSUP;
    return a->aio_wait(aio);
}
