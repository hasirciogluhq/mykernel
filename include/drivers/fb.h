#ifndef MYKERNEL_DRIVERS_FB_H
#define MYKERNEL_DRIVERS_FB_H

#include <kernel/types.h>
#include <multiboot.h>

typedef struct fb_info {
    uint8_t *addr;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t bytes_per_pixel;
    int      ready;
} fb_info_t;

int             fb_init(multiboot_info_t *mbi);
const fb_info_t *fb_get(void);
void            fb_put_pixel(uint32_t x, uint32_t y, uint32_t argb);
uint32_t        fb_get_pixel(uint32_t x, uint32_t y);
void            fb_fill(uint32_t argb);
void            fb_blit(uint32_t dx, uint32_t dy, const uint32_t *src,
                        uint32_t w, uint32_t h, uint32_t src_stride_px);
void            fb_present(const uint32_t *src, uint32_t src_stride_px);

#endif
