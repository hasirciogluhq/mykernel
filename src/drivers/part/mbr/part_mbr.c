#include <kernel/block_api.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/string.h>
#include <drivers/driver.h>
#include <drivers/vga.h>

typedef struct {
    uint8_t  status;
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t lba_start;
    uint32_t lba_sectors;
} __attribute__((packed)) mbr_part_t;

static int mbr_probe(block_device_t *disk)
{
    const block_api_t *api = block_api_get();
    uint8_t *sector;
    mbr_part_t *parts;
    int i, found = 0;

    if (!api || !disk || !api->read || !api->add_partition)
        return -EINVAL;

    sector = (uint8_t *)kmalloc(512);
    if (!sector)
        return -ENOMEM;
    if (api->read(disk, 0, sector, 1) < 0) {
        return 0;
    }
    if (sector[510] != 0x55 || sector[511] != 0xAA) {
        return 0;
    }
    /* Protective GPT MBR: type 0xEE → skip, let GPT win */
    parts = (mbr_part_t *)(sector + 446);
    if (parts[0].type == 0xEE)
        return 0;

    for (i = 0; i < 4; i++) {
        if (parts[i].type == 0 || parts[i].lba_sectors == 0)
            continue;
        if (api->add_partition(disk, i + 1,
                               parts[i].lba_start,
                               parts[i].lba_sectors, NULL) == 0)
            found++;
    }
    if (found)
        vga_print("part_mbr: partitions found\n");
    return found;
}

static const partition_ops_t g_mbr_ops = {
    .name = "mbr",
    .probe = mbr_probe,
};

static int mbr_init(driver_t *drv, void *ctx)
{
    const block_api_t *api = block_api_get();
    (void)drv;
    (void)ctx;
    if (!api || !api->register_partition_driver)
        return -1;
    return api->register_partition_driver(&g_mbr_ops);
}

int kmod_init(void)
{
    driver_t d;
    memset(&d, 0, sizeof(d));
    strncpy(d.name, "part_mbr", DRIVER_NAME_MAX - 1);
    strncpy(d.version, "1.0", DRIVER_VERSION_MAX - 1);
    d.kind = DRIVER_KIND_CUSTOM;
    d.class = DRIVER_CLASS_BUS;
    d.priority = 45;
    d.init = mbr_init;
    if (driver_register(&d) < 0)
        return -1;
    if (driver_load("part_mbr", NULL) < 0)
        return -1;
    return 0;
}
