#include <drivers/fb.h>
#include <arch/x86/io.h>
#include <kernel/string.h>

#define FB_DEFAULT_W   800
#define FB_DEFAULT_H   600
#define FB_DEFAULT_BPP 32

/* Bochs/QEMU Graphics Adapter (BGA) */
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

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC
#define PCI_VENDOR_BOCHS 0x1234
#define PCI_DEVICE_BGA   0x1111

/* Packed VBE 2.0 mode info (only fields we need) */
typedef struct vbe_mode_info {
    uint16_t attributes;
    uint8_t  win_a, win_b;
    uint16_t granularity;
    uint16_t winsize;
    uint16_t segment_a, segment_b;
    uint32_t real_fct_ptr;
    uint16_t pitch;
    uint16_t x_res, y_res;
    uint8_t  w_char, y_char, planes, bpp, banks;
    uint8_t  memory_model, bank_size, image_pages, reserved0;
    uint8_t  red_mask, red_pos, green_mask, green_pos;
    uint8_t  blue_mask, blue_pos, rsv_mask, rsv_pos;
    uint8_t  direct_color;
    uint32_t physbase;
} __attribute__((packed)) vbe_mode_info_t;

static fb_info_t fb;

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

static uint32_t pci_config_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    uint32_t addr = (1u << 31) |
                    ((uint32_t)bus << 16) |
                    ((uint32_t)slot << 11) |
                    ((uint32_t)func << 8) |
                    (offset & 0xFCu);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

static uint32_t bga_find_lfb(void)
{
    for (int bus = 0; bus < 8; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint32_t id = pci_config_read((uint8_t)bus, (uint8_t)slot, 0, 0);
            uint16_t vendor = (uint16_t)(id & 0xFFFF);
            uint16_t device = (uint16_t)(id >> 16);
            if (vendor == 0xFFFF)
                continue;
            if (vendor == PCI_VENDOR_BOCHS && device == PCI_DEVICE_BGA) {
                uint32_t bar0 = pci_config_read((uint8_t)bus, (uint8_t)slot, 0, 0x10);
                if (bar0 & 1u)
                    continue; /* I/O BAR */
                return bar0 & ~0xFu;
            }
        }
    }
    /* QEMU/Bochs historical default */
    return 0xE0000000u;
}

static int fb_try_multiboot_framebuffer(multiboot_info_t *mbi)
{
    if (!mbi || !(mbi->flags & MULTIBOOT_INFO_FRAMEBUFFER))
        return -1;
    if (mbi->framebuffer_type != MULTIBOOT_FRAMEBUFFER_TYPE_RGB)
        return -1;
    if (mbi->framebuffer_bpp != 32 && mbi->framebuffer_bpp != 24)
        return -1;
    if (mbi->framebuffer_width == 0 || mbi->framebuffer_height == 0)
        return -1;

    fb.addr = (uint8_t *)(uintptr_t)mbi->framebuffer_addr;
    fb.width = mbi->framebuffer_width;
    fb.height = mbi->framebuffer_height;
    fb.pitch = mbi->framebuffer_pitch;
    fb.bpp = mbi->framebuffer_bpp;
    fb.bytes_per_pixel = fb.bpp / 8;
    fb.ready = 1;
    return 0;
}

static int fb_try_multiboot_vbe(multiboot_info_t *mbi)
{
    if (!mbi || !(mbi->flags & MULTIBOOT_INFO_VBE) || !mbi->vbe_mode_info)
        return -1;

    vbe_mode_info_t *mode = (vbe_mode_info_t *)(uintptr_t)mbi->vbe_mode_info;
    if (!mode->physbase || mode->x_res == 0 || mode->y_res == 0)
        return -1;
    if (mode->bpp != 32 && mode->bpp != 24)
        return -1;

    fb.addr = (uint8_t *)(uintptr_t)mode->physbase;
    fb.width = mode->x_res;
    fb.height = mode->y_res;
    fb.pitch = mode->pitch;
    fb.bpp = mode->bpp;
    fb.bytes_per_pixel = fb.bpp / 8;
    fb.ready = 1;
    return 0;
}

static int fb_try_bga(uint32_t width, uint32_t height, uint32_t bpp)
{
    uint16_t id = bga_read(VBE_DISPI_INDEX_ID);
    if ((id & VBE_DISPI_ID_MASK) != VBE_DISPI_ID_MAGIC)
        return -1;

    bga_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    bga_write(VBE_DISPI_INDEX_XRES, (uint16_t)width);
    bga_write(VBE_DISPI_INDEX_YRES, (uint16_t)height);
    bga_write(VBE_DISPI_INDEX_BPP, (uint16_t)bpp);
    bga_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

    uint32_t lfb = bga_find_lfb();
    if (!lfb)
        return -1;

    fb.addr = (uint8_t *)(uintptr_t)lfb;
    fb.width = width;
    fb.height = height;
    fb.bpp = bpp;
    fb.bytes_per_pixel = bpp / 8;
    fb.pitch = width * fb.bytes_per_pixel;
    fb.ready = 1;
    return 0;
}

int fb_init(multiboot_info_t *mbi)
{
    memset(&fb, 0, sizeof(fb));

    if (fb_try_multiboot_framebuffer(mbi) == 0)
        return 0;
    if (fb_try_multiboot_vbe(mbi) == 0)
        return 0;
    if (fb_try_bga(FB_DEFAULT_W, FB_DEFAULT_H, FB_DEFAULT_BPP) == 0)
        return 0;

    return -1;
}

const fb_info_t *fb_get(void)
{
    return &fb;
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t argb)
{
    if (!fb.ready || x >= fb.width || y >= fb.height)
        return;

    uint8_t *p = fb.addr + y * fb.pitch + x * fb.bytes_per_pixel;
    if (fb.bytes_per_pixel == 4) {
        *(uint32_t *)p = argb;
    } else {
        p[0] = (uint8_t)(argb & 0xFF);
        p[1] = (uint8_t)((argb >> 8) & 0xFF);
        p[2] = (uint8_t)((argb >> 16) & 0xFF);
    }
}

uint32_t fb_get_pixel(uint32_t x, uint32_t y)
{
    if (!fb.ready || x >= fb.width || y >= fb.height)
        return 0;

    uint8_t *p = fb.addr + y * fb.pitch + x * fb.bytes_per_pixel;
    if (fb.bytes_per_pixel == 4)
        return *(uint32_t *)p;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | 0xFF000000u;
}

void fb_fill(uint32_t argb)
{
    if (!fb.ready)
        return;

    for (uint32_t y = 0; y < fb.height; y++) {
        uint8_t *row = fb.addr + y * fb.pitch;
        if (fb.bytes_per_pixel == 4) {
            uint32_t *px = (uint32_t *)row;
            for (uint32_t x = 0; x < fb.width; x++)
                px[x] = argb;
        } else {
            for (uint32_t x = 0; x < fb.width; x++) {
                uint8_t *p = row + x * 3;
                p[0] = (uint8_t)(argb & 0xFF);
                p[1] = (uint8_t)((argb >> 8) & 0xFF);
                p[2] = (uint8_t)((argb >> 16) & 0xFF);
            }
        }
    }
}

void fb_blit(uint32_t dx, uint32_t dy, const uint32_t *src,
             uint32_t w, uint32_t h, uint32_t src_stride_px)
{
    if (!fb.ready || !src)
        return;

    for (uint32_t y = 0; y < h; y++) {
        uint32_t sy = dy + y;
        if (sy >= fb.height)
            break;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t sx = dx + x;
            if (sx >= fb.width)
                break;
            fb_put_pixel(sx, sy, src[y * src_stride_px + x]);
        }
    }
}

void fb_present(const uint32_t *src, uint32_t src_stride_px)
{
    if (!fb.ready || !src)
        return;

    if (fb.bytes_per_pixel == 4 && fb.pitch == fb.width * 4 &&
        src_stride_px == fb.width) {
        memcpy(fb.addr, src, (size_t)fb.width * fb.height * 4);
        return;
    }

    for (uint32_t y = 0; y < fb.height; y++) {
        const uint32_t *srow = src + y * src_stride_px;
        uint8_t *drow = fb.addr + y * fb.pitch;
        if (fb.bytes_per_pixel == 4) {
            memcpy(drow, srow, fb.width * 4);
        } else {
            for (uint32_t x = 0; x < fb.width; x++) {
                uint32_t c = srow[x];
                uint8_t *p = drow + x * 3;
                p[0] = (uint8_t)(c & 0xFF);
                p[1] = (uint8_t)((c >> 8) & 0xFF);
                p[2] = (uint8_t)((c >> 16) & 0xFF);
            }
        }
    }
}

void fb_present_rect(const uint32_t *src, uint32_t src_stride_px,
                     uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!fb.ready || !src || w == 0 || h == 0)
        return;

    if (x >= fb.width || y >= fb.height)
        return;
    if (x + w > fb.width)
        w = fb.width - x;
    if (y + h > fb.height)
        h = fb.height - y;

    for (uint32_t row = 0; row < h; row++) {
        const uint32_t *srow = src + (y + row) * src_stride_px + x;
        uint8_t *drow = fb.addr + (y + row) * fb.pitch + x * fb.bytes_per_pixel;
        if (fb.bytes_per_pixel == 4) {
            memcpy(drow, srow, w * 4);
        } else {
            for (uint32_t col = 0; col < w; col++) {
                uint32_t c = srow[col];
                uint8_t *p = drow + col * 3;
                p[0] = (uint8_t)(c & 0xFF);
                p[1] = (uint8_t)((c >> 8) & 0xFF);
                p[2] = (uint8_t)((c >> 16) & 0xFF);
            }
        }
    }
}
