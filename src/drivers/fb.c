#include <drivers/fb.h>
#include <kernel/string.h>

static fb_info_t fb;

int fb_init(multiboot_info_t *mbi)
{
    memset(&fb, 0, sizeof(fb));

    if (!mbi || !(mbi->flags & MULTIBOOT_INFO_FRAMEBUFFER))
        return -1;

    if (mbi->framebuffer_type != MULTIBOOT_FRAMEBUFFER_TYPE_RGB)
        return -1;

    if (mbi->framebuffer_bpp != 32 && mbi->framebuffer_bpp != 24)
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
