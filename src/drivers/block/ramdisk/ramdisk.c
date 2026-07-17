#include <kernel/block_api.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/string.h>
#include <drivers/driver.h>
#include <drivers/vga.h>

#define RAMDISK_SECTORS  2048u /* 1 MiB */
#define RAMDISK_SECT     512u

typedef struct {
    uint8_t *data;
    size_t   bytes;
} ramdisk_priv_t;

static int rd_submit(bio_t *bio)
{
    ramdisk_priv_t *p;
    uint64_t off;
    size_t len;

    if (!bio || !bio->bdev)
        return -EINVAL;
    p = (ramdisk_priv_t *)bio->bdev->private_data;
    if (!p || !p->data)
        return -EIO;

    off = bio->sector * (uint64_t)bio->bdev->sector_size;
    len = bio->len;
    if (off + len > p->bytes) {
        bio->error = -EIO;
        bio->done = 1;
        if (bio->end_io)
            bio->end_io(bio);
        return -EIO;
    }

    if (bio->write)
        memcpy(p->data + (size_t)off, bio->data, len);
    else
        memcpy(bio->data, p->data + (size_t)off, len);

    bio->error = 0;
    bio->done = 1;
    if (bio->end_io)
        bio->end_io(bio);
    return 0;
}

static const block_ops_t rd_ops = {
    .submit_bio = rd_submit,
    .poll = NULL,
    .ioctl = NULL,
};

static block_device_t g_rd;
static ramdisk_priv_t g_priv;

static int ramdisk_init(driver_t *drv, void *ctx)
{
    const block_api_t *api = block_api_get();
    (void)drv;
    (void)ctx;
    if (!api || !api->add_disk)
        return -1;

    g_priv.bytes = (size_t)RAMDISK_SECTORS * RAMDISK_SECT;
    g_priv.data = (uint8_t *)kmalloc(g_priv.bytes);
    if (!g_priv.data)
        return -1;
    memset(g_priv.data, 0, g_priv.bytes);

    memset(&g_rd, 0, sizeof(g_rd));
    strcpy(g_rd.name, "ram0");
    g_rd.capacity = RAMDISK_SECTORS;
    g_rd.sector_size = RAMDISK_SECT;
    g_rd.ops = &rd_ops;
    g_rd.private_data = &g_priv;
    g_rd.major = 1;
    g_rd.minor = 0;

    if (api->add_disk(&g_rd) < 0)
        return -1;
    if (api->scan_partitions)
        api->scan_partitions(&g_rd);

    vga_print("ramdisk: ram0 ready\n");
    return 0;
}

int kmod_init(void)
{
    driver_t d;
    memset(&d, 0, sizeof(d));
    strncpy(d.name, "ramdisk", DRIVER_NAME_MAX - 1);
    strncpy(d.version, "1.0", DRIVER_VERSION_MAX - 1);
    d.kind = DRIVER_KIND_CUSTOM;
    d.class = DRIVER_CLASS_BUS;
    d.priority = 50;
    d.init = ramdisk_init;
    if (driver_register(&d) < 0)
        return -1;
    if (driver_load("ramdisk", NULL) < 0)
        return -1;
    return 0;
}
