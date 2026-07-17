#include "bga.h"
#include <drivers/display.h>
#include <drivers/driver.h>
#include <drivers/pci.h>
#include <drivers/ps2.h>
#include <drivers/serial.h>
#include <arch/x86/io.h>
#include <kernel/string.h>

#define BGA_DEFAULT_W   1280
#define BGA_DEFAULT_H   720
#define BGA_DEFAULT_BPP 32

#define VBE_DISPI_IOPORT_INDEX  0x01CE
#define VBE_DISPI_IOPORT_DATA   0x01CF
#define VBE_DISPI_INDEX_ID      0x0
#define VBE_DISPI_INDEX_XRES    0x1
#define VBE_DISPI_INDEX_YRES    0x2
#define VBE_DISPI_INDEX_BPP     0x3
#define VBE_DISPI_INDEX_ENABLE  0x4
#define VBE_DISPI_DISABLED      0x00
#define VBE_DISPI_ENABLED       0x01
#define VBE_DISPI_LFB_ENABLED   0x40
#define VBE_DISPI_ID_MAGIC      0xB0C0
#define VBE_DISPI_ID_MASK       0xFFF0

#define PCI_VENDOR_BOCHS 0x1234
#define PCI_DEVICE_BGA   0x1111

static display_mode_t g_mode;
static int g_ready;
static display_ops_t g_ops;

static void bga_write(uint16_t index, uint16_t value)
{
    outw(VBE_DISPI_IOPORT_INDEX, index);
    outw(VBE_DISPI_IOPORT_DATA, value);
}

static uint16_t bga_read(uint16_t index)
{
    outw(VBE_DISPI_IOPORT_INDEX, index);
    return inw(VBE_DISPI_IOPORT_DATA);
}

static uint32_t bga_find_lfb(void)
{
    pci_device_t dev;
    if (pci_find(PCI_VENDOR_BOCHS, PCI_DEVICE_BGA, &dev) == 0)
        return pci_bar_addr(dev.bar[0]);
    return 0xE0000000u;
}

static int bga_set_mode(uint32_t width, uint32_t height, uint32_t bpp)
{
    uint16_t id = bga_read(VBE_DISPI_INDEX_ID);
    uint32_t lfb;

    if ((id & VBE_DISPI_ID_MASK) != VBE_DISPI_ID_MAGIC)
        return -1;

    bga_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    bga_write(VBE_DISPI_INDEX_XRES, (uint16_t)width);
    bga_write(VBE_DISPI_INDEX_YRES, (uint16_t)height);
    bga_write(VBE_DISPI_INDEX_BPP, (uint16_t)bpp);
    bga_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

    lfb = bga_find_lfb();
    if (!lfb)
        return -1;

    memset(&g_mode, 0, sizeof(g_mode));
    g_mode.addr = (uint8_t *)(uintptr_t)lfb;
    g_mode.width = width;
    g_mode.height = height;
    g_mode.bpp = bpp;
    g_mode.bytes_per_pixel = bpp / 8;
    g_mode.pitch = width * g_mode.bytes_per_pixel;
    g_ready = 1;

    klog("[bga] mode ");
    serial_print_uint(width);
    klog("x");
    serial_print_uint(height);
    klog(" lfb=");
    serial_print_hex(lfb);
    klog("\n");

    /* Avoid pure black after leaving VGA text — proves LFB is alive. */
    if (g_mode.bytes_per_pixel == 4) {
        volatile uint32_t *px = (volatile uint32_t *)g_mode.addr;
        uint32_t n = width * height;
        uint32_t i;
        for (i = 0; i < n; i++)
            px[i] = 0xFF305C8Cu; /* desktop-ish blue */
    }
    return 0;
}

static int bga_get_mode(display_mode_t *out)
{
    if (!g_ready || !out)
        return -1;
    *out = g_mode;
    return 0;
}

static int bga_present(const uint32_t *src, uint32_t src_stride_px)
{
    uint32_t y, x;
    if (!g_ready || !src)
        return -1;

    /* Volatile dword stores — memcpy to MMIO LFB is unreliable on some QEMU builds. */
    if (g_mode.bytes_per_pixel == 4) {
        volatile uint32_t *dst = (volatile uint32_t *)g_mode.addr;
        uint32_t dst_stride = g_mode.pitch / 4u;
        for (y = 0; y < g_mode.height; y++) {
            const uint32_t *srow = src + y * src_stride_px;
            volatile uint32_t *drow = dst + y * dst_stride;
            /* Full-frame blit is long — keep the PS/2 FIFO from overflowing. */
            if ((y & 7) == 0)
                ps2_poll();
            for (x = 0; x < g_mode.width; x++)
                drow[x] = srow[x];
        }
        return 0;
    }

    for (y = 0; y < g_mode.height; y++) {
        const uint32_t *srow = src + y * src_stride_px;
        uint8_t *drow = g_mode.addr + y * g_mode.pitch;
        if ((y & 7) == 0)
            ps2_poll();
        for (x = 0; x < g_mode.width; x++) {
            uint32_t c = srow[x];
            uint8_t *p = drow + x * 3;
            p[0] = (uint8_t)(c & 0xFF);
            p[1] = (uint8_t)((c >> 8) & 0xFF);
            p[2] = (uint8_t)((c >> 16) & 0xFF);
        }
    }
    return 0;
}

static int bga_present_rect(const uint32_t *src, uint32_t src_stride_px,
                            uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t row;
    if (!g_ready || !src || w == 0 || h == 0)
        return -1;
    if (x >= g_mode.width || y >= g_mode.height)
        return -1;
    if (x + w > g_mode.width)
        w = g_mode.width - x;
    if (y + h > g_mode.height)
        h = g_mode.height - y;

    for (row = 0; row < h; row++) {
        const uint32_t *srow = src + (y + row) * src_stride_px + x;
        if (g_mode.bytes_per_pixel == 4) {
            volatile uint32_t *drow =
                (volatile uint32_t *)(g_mode.addr + (y + row) * g_mode.pitch + x * 4);
            uint32_t col = 0;
            /* Unrolled dword stores — MMIO-safe, better ILP than scalar loop. */
            for (; col + 4 <= w; col += 4) {
                drow[col] = srow[col];
                drow[col + 1] = srow[col + 1];
                drow[col + 2] = srow[col + 2];
                drow[col + 3] = srow[col + 3];
            }
            for (; col < w; col++)
                drow[col] = srow[col];
        } else {
            uint8_t *drow = g_mode.addr + (y + row) * g_mode.pitch +
                            x * g_mode.bytes_per_pixel;
            uint32_t col;
            for (col = 0; col < w; col++) {
                uint32_t c = srow[col];
                uint8_t *p = drow + col * 3;
                p[0] = (uint8_t)(c & 0xFF);
                p[1] = (uint8_t)((c >> 8) & 0xFF);
                p[2] = (uint8_t)((c >> 16) & 0xFF);
            }
        }
    }
    return 0;
}

static int bga_drv_probe(driver_t *drv, void *ctx)
{
    uint16_t id;
    (void)drv;
    (void)ctx;
    id = bga_read(VBE_DISPI_INDEX_ID);
    return ((id & VBE_DISPI_ID_MASK) == VBE_DISPI_ID_MAGIC) ? 0 : -1;
}

static int bga_drv_init(driver_t *drv, void *ctx)
{
    (void)drv;
    (void)ctx;
    if (bga_set_mode(BGA_DEFAULT_W, BGA_DEFAULT_H, BGA_DEFAULT_BPP) < 0)
        return -1;

    memset(&g_ops, 0, sizeof(g_ops));
    g_ops.name = "bga";
    g_ops.get_mode = bga_get_mode;
    g_ops.present = bga_present;
    g_ops.present_rect = bga_present_rect;
    g_ops.gpu_submit = NULL;
    return display_register(&g_ops, DISPLAY_PRIO_BGA);
}

int kmod_init(void)
{
    driver_t d;
    memset(&d, 0, sizeof(d));
    strncpy(d.name, "display_bga", DRIVER_NAME_MAX - 1);
    strncpy(d.version, "1.0", DRIVER_VERSION_MAX - 1);
    d.kind = DRIVER_KIND_CUSTOM;
    d.class = DRIVER_CLASS_DISPLAY;
    d.flags = 0;
    d.priority = 10;
    d.probe = bga_drv_probe;
    d.init = bga_drv_init;

    if (driver_register(&d) < 0)
        return -1;
    /* Soft-fail when BGA is absent so virtio-only boots can continue. */
    if (driver_load("display_bga", NULL) < 0)
        return 0;
    return 0;
}
