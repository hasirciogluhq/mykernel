#include "accel.h"
#include <kernel/string.h>

/*
 * Pixel-perfect rounded rect. Mirror into TL quadrant so all four corners
 * share one circle test — no asymmetric gaps from w-r-1 centers.
 */
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
    if (r <= 0)
        return 1;

    int32_t x = lx;
    int32_t y = ly;
    if (x >= w - r)
        x = w - 1 - x;
    if (y >= h - r)
        y = h - 1 - y;

    if (x >= r || y >= r)
        return 1;

    int32_t dx = r - x;
    int32_t dy = r - y;
    return dx * dx + dy * dy <= r * r;
}

uint8_t gx_round_coverage(int32_t lx, int32_t ly, int32_t w, int32_t h, int32_t r)
{
    if (lx < 0 || ly < 0 || lx >= w || ly >= h)
        return 0;
    if (r <= 0)
        return 255;
    if (r * 2 > w)
        r = w / 2;
    if (r * 2 > h)
        r = h / 2;
    if (r <= 0)
        return 255;

    int32_t x = lx;
    int32_t y = ly;
    if (x >= w - r)
        x = w - 1 - x;
    if (y >= h - r)
        y = h - 1 - y;

    if (x >= r || y >= r)
        return 255;

    /* 4×4 supersample against circle centered near (r-0.5, r-0.5). */
    int hits = 0;
    int32_t cx = r * 4 - 2;
    int32_t cy = r * 4 - 2;
    int32_t rr = cx * cx;
    for (int sy = 0; sy < 4; sy++) {
        for (int sx = 0; sx < 4; sx++) {
            int32_t px = x * 4 + sx;
            int32_t py = y * 4 + sy;
            int32_t dx = cx - px;
            int32_t dy = cy - py;
            if (dx * dx + dy * dy <= rr)
                hits++;
        }
    }
    return (uint8_t)((hits * 255 + 8) / 16);
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
            uint8_t cov = gx_round_coverage(x, y, r.w, r.h, radius);
            if (cov == 0)
                continue;
            int32_t dx = r.x + x;
            int32_t dy = r.y + y;
            if (dx < 0 || dy < 0 || (uint32_t)dx >= s->width || (uint32_t)dy >= s->height)
                continue;
            gx_color *dp = &s->pixels[(uint32_t)dy * s->stride + (uint32_t)dx];
            if (cov == 255 && GX_A(c) == 255)
                *dp = c;
            else
                *dp = gx_blend(*dp, gx_color_mul_alpha(c, cov));
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
        int32_t x = x0;
        while (x < src_r.w) {
            int32_t sx = src_r.x + x;
            int32_t dx2 = dx + x;
            if (sx < 0 || (uint32_t)sx >= src->width || dx2 < 0 ||
                (uint32_t)dx2 >= dst->width) {
                x++;
                continue;
            }

            gx_color c = src->pixels[(uint32_t)sy * src->stride + (uint32_t)sx];
            if (GX_A(c) == 0) {
                x++;
                continue;
            }
            if (GX_A(c) != 255) {
                gx_color *dp = &dst->pixels[(uint32_t)dy2 * dst->stride + (uint32_t)dx2];
                *dp = gx_blend(*dp, c);
                x++;
                continue;
            }

            /* Opaque run → memcpy (P09/P20). */
            int32_t run = 1;
            while (x + run < src_r.w) {
                int32_t sx2 = src_r.x + x + run;
                int32_t dx3 = dx + x + run;
                if (sx2 < 0 || (uint32_t)sx2 >= src->width || dx3 < 0 ||
                    (uint32_t)dx3 >= dst->width)
                    break;
                gx_color c2 = src->pixels[(uint32_t)sy * src->stride + (uint32_t)sx2];
                if (GX_A(c2) != 255)
                    break;
                run++;
            }
            memcpy(&dst->pixels[(uint32_t)dy2 * dst->stride + (uint32_t)dx2],
                   &src->pixels[(uint32_t)sy * src->stride + (uint32_t)sx],
                   (size_t)run * sizeof(gx_color));
            x += run;
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
