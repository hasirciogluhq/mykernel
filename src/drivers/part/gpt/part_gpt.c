#include <kernel/block_api.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/string.h>
#include <drivers/driver.h>
#include <drivers/vga.h>

typedef struct {
    char     sig[8];
    uint32_t rev;
    uint32_t hdr_size;
    uint32_t crc;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable;
    uint64_t last_usable;
    uint8_t  disk_guid[16];
    uint64_t part_lba;
    uint32_t part_count;
    uint32_t part_entry_size;
    uint32_t part_crc;
} __attribute__((packed)) gpt_header_t;

typedef struct {
    uint8_t  type_guid[16];
    uint8_t  uniq_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attrs;
    uint16_t name[36];
} __attribute__((packed)) gpt_entry_t;

static int guid_zero(const uint8_t *g)
{
    int i;
    for (i = 0; i < 16; i++)
        if (g[i])
            return 0;
    return 1;
}

static int gpt_probe(block_device_t *disk)
{
    const block_api_t *api = block_api_get();
    uint8_t *sector;
    gpt_header_t *hdr;
    uint8_t *ents;
    uint32_t i, found = 0;
    uint32_t nent, esz, bytes, nsect;

    if (!api || !disk || !api->read || !api->add_partition)
        return -EINVAL;

    sector = (uint8_t *)kmalloc(512);
    if (!sector)
        return -ENOMEM;
    if (api->read(disk, 1, sector, 1) < 0)
        return 0;
    hdr = (gpt_header_t *)sector;
    if (memcmp(hdr->sig, "EFI PART", 8) != 0)
        return 0;

    nent = hdr->part_count;
    esz = hdr->part_entry_size;
    if (nent == 0 || esz < sizeof(gpt_entry_t) || nent > 128)
        return 0;

    bytes = nent * esz;
    nsect = (bytes + 511) / 512;
    ents = (uint8_t *)kmalloc(nsect * 512);
    if (!ents)
        return -ENOMEM;
    if (api->read(disk, hdr->part_lba, ents, nsect) < 0)
        return 0;

    for (i = 0; i < nent; i++) {
        gpt_entry_t *e = (gpt_entry_t *)(ents + i * esz);
        uint64_t nsect_p;
        if (guid_zero(e->type_guid))
            continue;
        if (e->last_lba < e->first_lba)
            continue;
        nsect_p = e->last_lba - e->first_lba + 1;
        if (api->add_partition(disk, (int)i + 1, e->first_lba, nsect_p, NULL) == 0)
            found++;
    }
    if (found)
        vga_print("part_gpt: partitions found\n");
    return (int)found;
}

static const partition_ops_t g_gpt_ops = {
    .name = "gpt",
    .probe = gpt_probe,
};

static int gpt_init(driver_t *drv, void *ctx)
{
    const block_api_t *api = block_api_get();
    (void)drv;
    (void)ctx;
    if (!api || !api->register_partition_driver)
        return -1;
    return api->register_partition_driver(&g_gpt_ops);
}

int kmod_init(void)
{
    driver_t d;
    memset(&d, 0, sizeof(d));
    strncpy(d.name, "part_gpt", DRIVER_NAME_MAX - 1);
    strncpy(d.version, "1.0", DRIVER_VERSION_MAX - 1);
    d.kind = DRIVER_KIND_CUSTOM;
    d.class = DRIVER_CLASS_BUS;
    d.priority = 44;
    d.init = gpt_init;
    if (driver_register(&d) < 0)
        return -1;
    if (driver_load("part_gpt", NULL) < 0)
        return -1;
    return 0;
}
