#include <gfx/accel.h>

int gx_point_in_round_rect(int32_t lx, int32_t ly, int32_t w, int32_t h, int32_t r)
{
    if (lx < 0 || ly < 0 || lx >= w || ly >= h)
        return 0;
    if (r <= 0)
        return 1;
    if (r * 2 > w)
        r = w / 2;
    if (r * 2 > h)
        r = h / 2;

    /* center band */
    if (lx >= r && lx < w - r)
        return 1;
    if (ly >= r && ly < h - r)
        return 1;

    int32_t cx, cy;
    if (lx < r && ly < r) {
        cx = r;
        cy = r;
    } else if (lx >= w - r && ly < r) {
        cx = w - r - 1;
        cy = r;
    } else if (lx < r && ly >= h - r) {
        cx = r;
        cy = h - r - 1;
    } else {
        cx = w - r - 1;
        cy = h - r - 1;
    }

    int32_t dx = lx - cx;
    int32_t dy = ly - cy;
    return dx * dx + dy * dy <= r * r;
}

void gx_accel_fill(gx_surface *s, gx_rect r, gx_color c)
{
    if (!s || !s->pixels || gx_rect_empty(r))
        return;

    int32_t x0 = r.x < 0 ? 0 : r.x;
    int32_t y0 = r.y < 0 ? 0 : r.y;
    int32_t x1 = r.x + r.w;
    int32_t y1 = r.y + r.h;
    if (x1 > (int32_t)s->width)
        x1 = (int32_t)s->width;
    if (y1 > (int32_t)s->height)
        y1 = (int32_t)s->height;
    if (x0 >= x1 || y0 >= y1)
        return;

    for (int32_t y = y0; y < y1; y++) {
        gx_color *row = s->pixels + (uint32_t)y * s->stride + (uint32_t)x0;
        int32_t n = x1 - x0;
        /* dword blast */
        for (int32_t i = 0; i < n; i++)
            row[i] = c;
    }
}

void gx_accel_fill_round(gx_surface *s, gx_rect r, int32_t radius, gx_color c)
{
    if (!s || gx_rect_empty(r))
        return;
    if (radius <= 0) {
        gx_accel_fill(s, r, c);
        return;
    }

    for (int32_t y = 0; y < r.h; y++) {
        for (int32_t x = 0; x < r.w; x++) {
            if (!gx_point_in_round_rect(x, y, r.w, r.h, radius))
                continue;
            int32_t dx = r.x + x;
            int32_t dy = r.y + y;
            if (dx < 0 || dy < 0 || (uint32_t)dx >= s->width || (uint32_t)dy >= s->height)
                continue;
            s->pixels[(uint32_t)dy * s->stride + (uint32_t)dx] = c;
        }
    }
}

void gx_accel_blit_rect(gx_surface *dst, int32_t dx, int32_t dy,
                        const gx_surface *src, gx_rect src_r)
{
    if (!dst || !src || gx_rect_empty(src_r))
        return;

    for (int32_t y = 0; y < src_r.h; y++) {
        int32_t sy = src_r.y + y;
        int32_t dy2 = dy + y;
        if (sy < 0 || (uint32_t)sy >= src->height || dy2 < 0 || (uint32_t)dy2 >= dst->height)
            continue;

        int32_t x0 = src_r.x < 0 ? -src_r.x : 0;
        for (int32_t x = x0; x < src_r.w; x++) {
            int32_t sx = src_r.x + x;
            int32_t dx2 = dx + x;
            if (sx < 0 || (uint32_t)sx >= src->width || dx2 < 0 || (uint32_t)dx2 >= dst->width)
                continue;
            gx_color c = src->pixels[(uint32_t)sy * src->stride + (uint32_t)sx];
            if (GX_A(c) == 0)
                continue;
            if (GX_A(c) == 255)
                dst->pixels[(uint32_t)dy2 * dst->stride + (uint32_t)dx2] = c;
            else {
                gx_color *dp = &dst->pixels[(uint32_t)dy2 * dst->stride + (uint32_t)dx2];
                *dp = gx_blend(*dp, c);
            }
        }
    }
}

void gx_accel_blit(gx_surface *dst, int32_t dx, int32_t dy, const gx_surface *src)
{
    if (!dst || !src)
        return;
    gx_accel_blit_rect(dst, dx, dy, src,
                       gx_rect_make(0, 0, (int32_t)src->width, (int32_t)src->height));
}
