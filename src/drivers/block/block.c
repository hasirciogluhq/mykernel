#include <kernel/block_api.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/string.h>
#include <drivers/driver.h>
#include <drivers/vga.h>

#define BLOCK_MAX_DISKS   32
#define BLOCK_MAX_PARTDRV 8
#define BIO_QUEUE_MAX     64

typedef struct part_priv {
    block_device_t *parent;
    uint64_t        start;
    void           *parent_priv;
} part_priv_t;

static block_device_t *g_disks[BLOCK_MAX_DISKS];
static size_t g_disk_count;
static const partition_ops_t *g_part[BLOCK_MAX_PARTDRV];
static size_t g_part_count;
static bio_t *g_pending[BIO_QUEUE_MAX];
static size_t g_pending_count;

static int part_submit(bio_t *bio);
static int part_poll(block_device_t *bdev);

static const block_ops_t g_part_ops = {
    .submit_bio = part_submit,
    .poll = part_poll,
    .ioctl = NULL,
};

static int register_blkdev(int major, const char *name)
{
    (void)major;
    (void)name;
    return 0;
}

static int add_disk(block_device_t *bdev)
{
    size_t i;
    if (!bdev || !bdev->ops || !bdev->ops->submit_bio)
        return -EINVAL;
    if (g_disk_count >= BLOCK_MAX_DISKS)
        return -ENOSPC;
    for (i = 0; i < g_disk_count; i++) {
        if (strcmp(g_disks[i]->name, bdev->name) == 0)
            return -EEXIST;
    }
    if (bdev->sector_size == 0)
        bdev->sector_size = 512;
    g_disks[g_disk_count++] = bdev;
    vga_print("block: disk ");
    vga_print(bdev->name);
    vga_print("\n");
    return 0;
}

static int del_disk(block_device_t *bdev)
{
    size_t i, j;
    if (!bdev)
        return -EINVAL;
    for (i = 0; i < g_disk_count; i++) {
        if (g_disks[i] == bdev) {
            for (j = i; j + 1 < g_disk_count; j++)
                g_disks[j] = g_disks[j + 1];
            g_disk_count--;
            return 0;
        }
    }
    return -ENOENT;
}

static block_device_t *lookup(const char *name)
{
    size_t i;
    if (!name)
        return NULL;
    for (i = 0; i < g_disk_count; i++) {
        if (strcmp(g_disks[i]->name, name) == 0)
            return g_disks[i];
    }
    return NULL;
}

static int submit_bio(bio_t *bio)
{
    int rc;
    if (!bio || !bio->bdev || !bio->bdev->ops || !bio->bdev->ops->submit_bio)
        return -EINVAL;
    bio->done = 0;
    bio->error = 0;
    rc = bio->bdev->ops->submit_bio(bio);
    if (rc < 0) {
        bio->error = rc;
        bio->done = 1;
        if (bio->end_io)
            bio->end_io(bio);
        return rc;
    }
    if (!bio->done && g_pending_count < BIO_QUEUE_MAX)
        g_pending[g_pending_count++] = bio;
    return 0;
}

static int block_poll_all(void)
{
    size_t i, w;
    int n = 0;

    for (i = 0; i < g_disk_count; i++) {
        if (g_disks[i]->ops && g_disks[i]->ops->poll)
            n += g_disks[i]->ops->poll(g_disks[i]);
    }

    w = 0;
    for (i = 0; i < g_pending_count; i++) {
        if (!g_pending[i]->done)
            g_pending[w++] = g_pending[i];
    }
    g_pending_count = w;
    return n;
}

static int submit_bio_sync(bio_t *bio)
{
    int rc = submit_bio(bio);
    int spins = 0;
    if (rc < 0)
        return rc;
    while (!bio->done && spins < 1000000) {
        block_poll_all();
        spins++;
    }
    if (!bio->done)
        return -EIO;
    return bio->error;
}

static int blk_read(block_device_t *bdev, uint64_t sector, void *buf, size_t nsect)
{
    bio_t bio;
    if (!bdev || !buf || nsect == 0)
        return -EINVAL;
    memset(&bio, 0, sizeof(bio));
    bio.bdev = bdev;
    bio.sector = sector;
    bio.data = buf;
    bio.len = nsect * (size_t)bdev->sector_size;
    bio.write = 0;
    return submit_bio_sync(&bio);
}

static int blk_write(block_device_t *bdev, uint64_t sector, const void *buf, size_t nsect)
{
    bio_t bio;
    if (!bdev || !buf || nsect == 0)
        return -EINVAL;
    memset(&bio, 0, sizeof(bio));
    bio.bdev = bdev;
    bio.sector = sector;
    bio.data = (void *)buf;
    bio.len = nsect * (size_t)bdev->sector_size;
    bio.write = 1;
    return submit_bio_sync(&bio);
}

static int register_partition_driver(const partition_ops_t *ops)
{
    if (!ops || !ops->probe || !ops->name)
        return -EINVAL;
    if (g_part_count >= BLOCK_MAX_PARTDRV)
        return -ENOSPC;
    g_part[g_part_count++] = ops;
    return 0;
}

static int part_submit(bio_t *bio)
{
    part_priv_t *pp;
    bio_t nested;
    int rc;

    if (!bio || !bio->bdev)
        return -EINVAL;
    pp = (part_priv_t *)bio->bdev->private_data;
    if (!pp || !pp->parent || !pp->parent->ops || !pp->parent->ops->submit_bio)
        return -EIO;

    nested = *bio;
    nested.bdev = pp->parent;
    nested.sector = bio->sector + pp->start;
    nested.done = 0;
    nested.error = 0;
    /* Keep end_io on original; nested completes by copying state. */
    nested.end_io = NULL;
    rc = pp->parent->ops->submit_bio(&nested);
    if (rc < 0)
        return rc;
    bio->done = nested.done;
    bio->error = nested.error;
    if (bio->done && bio->end_io)
        bio->end_io(bio);
    return 0;
}

static int part_poll(block_device_t *bdev)
{
    part_priv_t *pp = (part_priv_t *)bdev->private_data;
    if (pp && pp->parent && pp->parent->ops && pp->parent->ops->poll)
        return pp->parent->ops->poll(pp->parent);
    return 0;
}

static int add_partition_fixed(block_device_t *parent, int partno,
                               uint64_t start_sect, uint64_t nsect, const char *name)
{
    block_device_t *part;
    part_priv_t *pp;
    char auto_name[32];

    if (!parent || nsect == 0)
        return -EINVAL;
    part = (block_device_t *)kmalloc(sizeof(*part));
    pp = (part_priv_t *)kmalloc(sizeof(*pp));
    if (!part || !pp)
        return -ENOMEM;
    memset(part, 0, sizeof(*part));
    memset(pp, 0, sizeof(*pp));
    pp->parent = parent;
    pp->start = start_sect;
    pp->parent_priv = parent->private_data;

    if (name && name[0]) {
        strncpy(part->name, name, sizeof(part->name) - 1);
    } else {
        size_t n = strlen(parent->name);
        if (n >= sizeof(auto_name) - 4)
            n = sizeof(auto_name) - 4;
        memcpy(auto_name, parent->name, n);
        if (partno >= 10) {
            auto_name[n] = 'p';
            auto_name[n + 1] = (char)('0' + (partno / 10));
            auto_name[n + 2] = (char)('0' + (partno % 10));
            auto_name[n + 3] = 0;
        } else {
            auto_name[n] = 'p';
            auto_name[n + 1] = (char)('0' + partno);
            auto_name[n + 2] = 0;
        }
        strncpy(part->name, auto_name, sizeof(part->name) - 1);
    }

    part->capacity = nsect;
    part->sector_size = parent->sector_size ? parent->sector_size : 512;
    part->ops = &g_part_ops;
    part->private_data = pp;
    part->major = parent->major;
    part->minor = parent->minor + partno;
    part->partition = partno;
    return add_disk(part);
}

static int scan_partitions(block_device_t *disk)
{
    size_t i;
    if (!disk || disk->partition)
        return -EINVAL;
    for (i = 0; i < g_part_count; i++) {
        int rc = g_part[i]->probe(disk);
        if (rc > 0)
            return rc;
    }
    return 0;
}

static size_t disk_count(void)
{
    return g_disk_count;
}

static block_device_t *disk_at(size_t index)
{
    if (index >= g_disk_count)
        return NULL;
    return g_disks[index];
}

static const block_api_t g_api = {
    .register_blkdev = register_blkdev,
    .add_disk = add_disk,
    .del_disk = del_disk,
    .add_partition = add_partition_fixed,
    .lookup = lookup,
    .submit_bio = submit_bio,
    .submit_bio_sync = submit_bio_sync,
    .read = blk_read,
    .write = blk_write,
    .register_partition_driver = register_partition_driver,
    .scan_partitions = scan_partitions,
    .poll = block_poll_all,
    .disk_count = disk_count,
    .disk_at = disk_at,
};

static int block_drv_init(driver_t *drv, void *ctx)
{
    (void)drv;
    (void)ctx;
    block_api_register(&g_api);
    vga_print("block: layer ready\n");
    return 0;
}

int kmod_init(void)
{
    driver_t d;
    memset(&d, 0, sizeof(d));
    strncpy(d.name, "block", DRIVER_NAME_MAX - 1);
    strncpy(d.version, "1.0", DRIVER_VERSION_MAX - 1);
    d.kind = DRIVER_KIND_CUSTOM;
    d.class = DRIVER_CLASS_BUS;
    d.flags = 0;
    d.priority = 10;
    d.init = block_drv_init;
    if (driver_register(&d) < 0)
        return -1;
    if (driver_load("block", NULL) < 0)
        return -1;
    return 0;
}
