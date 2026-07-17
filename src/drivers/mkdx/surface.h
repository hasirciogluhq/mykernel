#ifndef MYKERNEL_GFX_SURFACE_H
#define MYKERNEL_GFX_SURFACE_H

#include "color.h"

typedef struct gx_rect {
    int32_t x, y;
    int32_t w, h;
} gx_rect;

typedef struct gx_surface {
    uint32_t  width;
    uint32_t  height;
    uint32_t  stride; /* pixels per row */
    gx_color *pixels;
    int       owned;  /* 1 = free pixels on destroy */
} gx_surface;

gx_surface *gx_surface_create(uint32_t w, uint32_t h);
gx_surface *gx_surface_wrap(gx_color *pixels, uint32_t w, uint32_t h, uint32_t stride);
void        gx_surface_destroy(gx_surface *s);
void        gx_surface_clear(gx_surface *s, gx_color c);
gx_color    gx_surface_get(const gx_surface *s, int32_t x, int32_t y);
void        gx_surface_set(gx_surface *s, int32_t x, int32_t y, gx_color c);

static inline int gx_rect_empty(gx_rect r)
{
    return r.w <= 0 || r.h <= 0;
}

gx_rect gx_rect_intersect(gx_rect a, gx_rect b);
gx_rect gx_rect_union(gx_rect a, gx_rect b);
gx_rect gx_rect_make(int32_t x, int32_t y, int32_t w, int32_t h);
static inline int32_t gx_rect_area(gx_rect r)
{
    if (gx_rect_empty(r))
        return 0;
    return r.w * r.h;
}

#endif
