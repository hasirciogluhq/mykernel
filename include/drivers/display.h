#ifndef MYKERNEL_DRIVERS_DISPLAY_H
#define MYKERNEL_DRIVERS_DISPLAY_H

#include <kernel/types.h>

#define DISPLAY_PRIO_BGA    10
#define DISPLAY_PRIO_VIRTIO 20

typedef struct display_mode {
    uint8_t *addr;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t bytes_per_pixel;
} display_mode_t;

typedef struct display_rect {
    uint32_t x, y, w, h;
} display_rect_t;

typedef struct display_ops {
    const char *name;
    int (*get_mode)(display_mode_t *out);
    int (*present)(const uint32_t *src, uint32_t src_stride_px);
    int (*present_rect)(const uint32_t *src, uint32_t src_stride_px,
                         uint32_t x, uint32_t y, uint32_t w, uint32_t h);
    /*
     * Optional batch present: copy N rects then one host flush.
     * Used by drag to avoid N sync TRANSFER+FLUSH round-trips.
     * NULL = caller falls back to present_rect per region.
     */
    int (*present_rects)(const uint32_t *src, uint32_t src_stride_px,
                         const display_rect_t *rects, uint32_t n);
    /* Optional GPU submit path (virtio). NULL = software only. */
    int (*gpu_submit)(const void *cmd, uint32_t size);
} display_ops_t;

void           display_framework_init(void);
int            display_register(display_ops_t *ops, int priority);
void           display_unregister(display_ops_t *ops);
display_ops_t *display_active(void);
int            display_get_screen_size(uint32_t *w, uint32_t *h, uint32_t *bpp);

#endif
