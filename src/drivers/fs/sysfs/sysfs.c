#include <kernel/vfs_api.h>
#include <kernel/errno.h>
#include <kernel/string.h>
#include <drivers/vfs_fs.h>
#include <drivers/driver.h>
#include <drivers/vga.h>

static int sysfs_mount(file_system_type_t *fs_type, int flags,
                         const char *dev_name, void *data, super_block_t **sb_out)
{
    (void)fs_type; (void)flags; (void)dev_name; (void)data; (void)sb_out;
    return -ENOTSUP;
}

static file_system_type_t g_fs = {
    .name = "sysfs",
    .mount = sysfs_mount,
};

static int sysfs_init(driver_t *drv, void *ctx)
{
    const vfs_api_t *api = vfs_api_get();
    (void)drv; (void)ctx;
    if (!api || !api->register_filesystem)
        return -1;
    if (api->register_filesystem(&g_fs) < 0)
        return -1;
    vga_print("sysfs: registered\n");
    return 0;
}

int kmod_init(void)
{
    driver_t d;
    memset(&d, 0, sizeof(d));
    strncpy(d.name, "sysfs", DRIVER_NAME_MAX - 1);
    strncpy(d.version, "0.1", DRIVER_VERSION_MAX - 1);
    d.kind = DRIVER_KIND_CUSTOM;
    d.class = DRIVER_CLASS_MISC;
    d.priority = 70;
    d.init = sysfs_init;
    if (driver_register(&d) < 0) return -1;
    if (driver_load("sysfs", NULL) < 0) return -1;
    return 0;
}
