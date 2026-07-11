#include <gfx/surface.h>
#include <kernel/heap.h>
#include <kernel/string.h>

gx_rect gx_rect_make(int32_t x, int32_t y, int32_t w, int32_t h)
{
    gx_rect r;
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    return r;
}

gx_rect gx_rect_intersect(gx_rect a, gx_rect b)
{
    int32_t x0 = a.x > b.x ? a.x : b.x;
    int32_t y0 = a.y > b.y ? a.y : b.y;
    int32_t x1 = (a.x + a.w) < (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int32_t y1 = (a.y + a.h) < (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    if (x1 <= x0 || y1 <= y0)
        return gx_rect_make(0, 0, 0, 0);
    return gx_rect_make(x0, y0, x1 - x0, y1 - y0);
}

gx_surface *gx_surface_create(uint32_t w, uint32_t h)
{
    if (w == 0 || h == 0)
        return NULL;

    gx_surface *s = kmalloc(sizeof(gx_surface));
    if (!s)
        return NULL;

    size_t bytes = (size_t)w * (size_t)h * sizeof(gx_color);
    gx_color *px = kmalloc_aligned(bytes, 16);
    if (!px) {
        kfree(s);
        return NULL;
    }

    s->width = w;
    s->height = h;
    s->stride = w;
    s->pixels = px;
    s->owned = 1;
    memset(px, 0, bytes);
    return s;
}

gx_surface *gx_surface_wrap(gx_color *pixels, uint32_t w, uint32_t h, uint32_t stride)
{
    gx_surface *s = kmalloc(sizeof(gx_surface));
    if (!s)
        return NULL;
    s->width = w;
    s->height = h;
    s->stride = stride;
    s->pixels = pixels;
    s->owned = 0;
    return s;
}

void gx_surface_destroy(gx_surface *s)
{
    if (!s)
        return;
    if (s->owned && s->pixels)
        kfree(s->pixels);
    kfree(s);
}

void gx_surface_clear(gx_surface *s, gx_color c)
{
    if (!s || !s->pixels)
        return;
    size_t n = (size_t)s->stride * s->height;
    for (size_t i = 0; i < n; i++)
        s->pixels[i] = c;
}

gx_color gx_surface_get(const gx_surface *s, int32_t x, int32_t y)
{
    if (!s || x < 0 || y < 0 || (uint32_t)x >= s->width || (uint32_t)y >= s->height)
        return GX_TRANSPARENT;
    return s->pixels[(uint32_t)y * s->stride + (uint32_t)x];
}

void gx_surface_set(gx_surface *s, int32_t x, int32_t y, gx_color c)
{
    if (!s || x < 0 || y < 0 || (uint32_t)x >= s->width || (uint32_t)y >= s->height)
        return;
    s->pixels[(uint32_t)y * s->stride + (uint32_t)x] = c;
}
