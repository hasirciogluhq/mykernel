#include <kernel/block_api.h>
#include <kernel/errno.h>
#include <kernel/string.h>
#include <drivers/driver.h>
#include <drivers/vga.h>
#include <drivers/pci.h>

static int ahci_probe_init(driver_t *drv, void *ctx)
{
    (void)drv; (void)ctx;
    /* Hardware probe left for later; register soft-ready stub. */
    if (!block_api_get())
        return -1;
    vga_print("ahci: stub registered (no device)\n");
    return 0;
}

int kmod_init(void)
{
    driver_t d;
    memset(&d, 0, sizeof(d));
    strncpy(d.name, "ahci", DRIVER_NAME_MAX - 1);
    strncpy(d.version, "0.1", DRIVER_VERSION_MAX - 1);
    d.kind = DRIVER_KIND_CUSTOM;
    d.class = DRIVER_CLASS_BUS;
    d.priority = 80;
    d.init = ahci_probe_init;
    if (driver_register(&d) < 0) return -1;
    /* Soft-fail if no hardware */
    if (driver_load("ahci", NULL) < 0)
        return 0;
    return 0;
}
