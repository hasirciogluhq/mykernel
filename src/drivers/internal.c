#include <drivers/driver.h>
#include <drivers/vga.h>
#include <drivers/fb.h>
#include <drivers/ps2.h>
#include <drivers/keyboard.h>
#include <drivers/mouse.h>
#include <drivers/console.h>
#include <kernel/string.h>
#include <multiboot.h>

/* ---- VGA (early, already running before framework) ---- */

static int vga_drv_init(driver_t *drv, void *ctx)
{
    (void)drv;
    (void)ctx;
    vga_init();
    return 0;
}

/* ---- Framebuffer ---- */

static int fb_drv_probe(driver_t *drv, void *ctx)
{
    (void)drv;
    return ctx ? 0 : -1;
}

static int fb_drv_init(driver_t *drv, void *ctx)
{
    (void)drv;
    return fb_init((multiboot_info_t *)ctx);
}

/* ---- PS/2 bus ---- */

static int ps2_drv_init(driver_t *drv, void *ctx)
{
    (void)drv;
    (void)ctx;
    ps2_init();
    return 0;
}

static void ps2_drv_poll(driver_t *drv)
{
    (void)drv;
    ps2_poll();
}

/* ---- Keyboard ---- */

static int keyboard_drv_init(driver_t *drv, void *ctx)
{
    (void)drv;
    (void)ctx;
    keyboard_init();
    return 0;
}

/* ---- Mouse ---- */

static int mouse_drv_init(driver_t *drv, void *ctx)
{
    (void)drv;
    (void)ctx;
    mouse_init();
    return 0;
}

/* ---- Console ---- */

static int console_drv_init(driver_t *drv, void *ctx)
{
    (void)drv;
    (void)ctx;
    console_init();
    return 0;
}

static void fill_name(driver_t *d, const char *name, const char *ver)
{
    memset(d, 0, sizeof(*d));
    strncpy(d->name, name, DRIVER_NAME_MAX - 1);
    strncpy(d->version, ver, DRIVER_VERSION_MAX - 1);
}

void drivers_register_internal(void)
{
    driver_t d;

    fill_name(&d, "vga", "1.0");
    d.kind = DRIVER_KIND_INTERNAL;
    d.class = DRIVER_CLASS_DISPLAY;
    d.flags = DRIVER_FLAG_EARLY;
    d.priority = 0;
    d.init = vga_drv_init;
    driver_register(&d);

    fill_name(&d, "console", "1.0");
    d.kind = DRIVER_KIND_INTERNAL;
    d.class = DRIVER_CLASS_CHAR;
    d.flags = DRIVER_FLAG_AUTO;
    d.priority = 5;
    d.init = console_drv_init;
    driver_register(&d);

    fill_name(&d, "fb", "1.0");
    d.kind = DRIVER_KIND_INTERNAL;
    d.class = DRIVER_CLASS_DISPLAY;
    d.flags = DRIVER_FLAG_AUTO;
    d.priority = 10;
    d.probe = fb_drv_probe;
    d.init = fb_drv_init;
    driver_register(&d);

    fill_name(&d, "ps2", "1.0");
    d.kind = DRIVER_KIND_INTERNAL;
    d.class = DRIVER_CLASS_BUS;
    d.flags = DRIVER_FLAG_AUTO | DRIVER_FLAG_POLL;
    d.priority = 20;
    d.init = ps2_drv_init;
    d.poll = ps2_drv_poll;
    driver_register(&d);

    fill_name(&d, "keyboard", "1.0");
    d.kind = DRIVER_KIND_INTERNAL;
    d.class = DRIVER_CLASS_INPUT;
    d.flags = DRIVER_FLAG_AUTO;
    d.priority = 30;
    d.init = keyboard_drv_init;
    driver_register(&d);

    fill_name(&d, "mouse", "1.0");
    d.kind = DRIVER_KIND_INTERNAL;
    d.class = DRIVER_CLASS_INPUT;
    d.flags = DRIVER_FLAG_AUTO;
    d.priority = 40;
    d.init = mouse_drv_init;
    driver_register(&d);
}
