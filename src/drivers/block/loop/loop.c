#include <kernel/block_api.h>
#include <kernel/vfs_api.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/string.h>
#include <drivers/driver.h>
#include <drivers/vga.h>

#define LOOP_MAX 4
#define LOOP_SECT 512

typedef struct {
    int      used;
    int      fd;
    uint64_t capacity;
    block_device_t bdev;
    char     name[16];
} loop_dev_t;

static loop_dev_t g_loops[LOOP_MAX];

static int loop_submit(bio_t *bio)
{
    loop_dev_t *lp;
    const vfs_api_t *vfs;
    off_t off;
    ssize_t n;

    if (!bio || !bio->bdev)
        return -EINVAL;
    lp = (loop_dev_t *)bio->bdev->private_data;
    vfs = vfs_api_get();
    if (!lp || !vfs)
        return -EIO;

    off = (off_t)(bio->sector * (uint64_t)LOOP_SECT);
    if (vfs->lseek)
        vfs->lseek(lp->fd, off, 0);

    if (bio->write) {
        if (!vfs->write)
            return -ENOTSUP;
        n = vfs->write(lp->fd, bio->data, bio->len);
    } else {
        if (!vfs->read)
            return -ENOTSUP;
        n = vfs->read(lp->fd, bio->data, bio->len);
    }

    if (n < 0) {
        bio->error = (int)n;
        bio->done = 1;
        if (bio->end_io)
            bio->end_io(bio);
        return (int)n;
    }
    bio->error = 0;
    bio->done = 1;
    if (bio->end_io)
        bio->end_io(bio);
    return 0;
}

static const block_ops_t loop_ops = {
    .submit_bio = loop_submit,
};

/* Attach path → loopN. Called via ioctl-like helper exported through block. */
int loop_attach(const char *path, char *out_name, size_t out_len)
{
    const vfs_api_t *vfs = vfs_api_get();
    const block_api_t *blk = block_api_get();
    int i, fd;
    loop_dev_t *lp;

    if (!vfs || !blk || !path)
        return -EINVAL;
    for (i = 0; i < LOOP_MAX; i++) {
        if (!g_loops[i].used)
            break;
    }
    if (i >= LOOP_MAX)
        return -ENOSPC;

    fd = vfs->open(path, 2 /* O_RDWR */);
    if (fd < 0)
        return fd;

    lp = &g_loops[i];
    memset(lp, 0, sizeof(*lp));
    lp->used = 1;
    lp->fd = fd;
    lp->capacity = 4096; /* default 2MiB until stat */
    if (vfs->fstat) {
        /* optional size via fstat if available — keep default */
    }
    lp->name[0] = 'l';
    lp->name[1] = 'o';
    lp->name[2] = 'o';
    lp->name[3] = 'p';
    lp->name[4] = (char)('0' + i);
    lp->name[5] = 0;

    memset(&lp->bdev, 0, sizeof(lp->bdev));
    strncpy(lp->bdev.name, lp->name, sizeof(lp->bdev.name) - 1);
    lp->bdev.capacity = lp->capacity;
    lp->bdev.sector_size = LOOP_SECT;
    lp->bdev.ops = &loop_ops;
    lp->bdev.private_data = lp;
    lp->bdev.major = 7;
    lp->bdev.minor = i;

    if (blk->add_disk(&lp->bdev) < 0) {
        vfs->close(fd);
        lp->used = 0;
        return -EIO;
    }
    if (out_name && out_len)
        strncpy(out_name, lp->name, out_len - 1);
    return 0;
}

static int loop_init(driver_t *drv, void *ctx)
{
    (void)drv;
    (void)ctx;
    memset(g_loops, 0, sizeof(g_loops));
    vga_print("loop: ready\n");
    return 0;
}

int kmod_init(void)
{
    driver_t d;
    memset(&d, 0, sizeof(d));
    strncpy(d.name, "loop", DRIVER_NAME_MAX - 1);
    strncpy(d.version, "1.0", DRIVER_VERSION_MAX - 1);
    d.kind = DRIVER_KIND_CUSTOM;
    d.class = DRIVER_CLASS_BUS;
    d.priority = 55;
    d.init = loop_init;
    if (driver_register(&d) < 0)
        return -1;
    if (driver_load("loop", NULL) < 0)
        return -1;
    return 0;
}
