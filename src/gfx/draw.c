#include <gfx/draw.h>

static int clampi(int v, int lo, int hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

void gx_fill_rect(gx_surface *s, gx_rect r, gx_color c)
{
    if (!s || gx_rect_empty(r))
        return;

    int32_t x0 = clampi(r.x, 0, (int)s->width);
    int32_t y0 = clampi(r.y, 0, (int)s->height);
    int32_t x1 = clampi(r.x + r.w, 0, (int)s->width);
    int32_t y1 = clampi(r.y + r.h, 0, (int)s->height);

    for (int32_t y = y0; y < y1; y++) {
        gx_color *row = s->pixels + (uint32_t)y * s->stride;
        for (int32_t x = x0; x < x1; x++)
            row[x] = c;
    }
}

void gx_fill_rect_aa(gx_surface *s, gx_rect r, gx_color c)
{
    if (!s || gx_rect_empty(r))
        return;
    if (GX_A(c) == 255) {
        gx_fill_rect(s, r, c);
        return;
    }

    int32_t x0 = clampi(r.x, 0, (int)s->width);
    int32_t y0 = clampi(r.y, 0, (int)s->height);
    int32_t x1 = clampi(r.x + r.w, 0, (int)s->width);
    int32_t y1 = clampi(r.y + r.h, 0, (int)s->height);

    for (int32_t y = y0; y < y1; y++) {
        gx_color *row = s->pixels + (uint32_t)y * s->stride;
        for (int32_t x = x0; x < x1; x++)
            row[x] = gx_blend(row[x], c);
    }
}

void gx_draw_rect(gx_surface *s, gx_rect r, gx_color c, int thickness)
{
    if (thickness < 1)
        thickness = 1;
    gx_fill_rect(s, gx_rect_make(r.x, r.y, r.w, thickness), c);
    gx_fill_rect(s, gx_rect_make(r.x, r.y + r.h - thickness, r.w, thickness), c);
    gx_fill_rect(s, gx_rect_make(r.x, r.y, thickness, r.h), c);
    gx_fill_rect(s, gx_rect_make(r.x + r.w - thickness, r.y, thickness, r.h), c);
}

void gx_draw_line(gx_surface *s, int32_t x0, int32_t y0, int32_t x1, int32_t y1, gx_color c)
{
    int32_t dx = x1 - x0;
    int32_t dy = y1 - y0;
    int32_t ax = dx < 0 ? -dx : dx;
    int32_t ay = dy < 0 ? -dy : dy;
    int32_t sx = dx < 0 ? -1 : 1;
    int32_t sy = dy < 0 ? -1 : 1;
    int32_t err = ax - ay;

    for (;;) {
        gx_surface_set(s, x0, y0, c);
        if (x0 == x1 && y0 == y1)
            break;
        int32_t e2 = err * 2;
        if (e2 > -ay) {
            err -= ay;
            x0 += sx;
        }
        if (e2 < ax) {
            err += ax;
            y0 += sy;
        }
    }
}

void gx_fill_circle(gx_surface *s, int32_t cx, int32_t cy, int32_t radius, gx_color c)
{
    if (radius <= 0)
        return;
    int32_t r2 = radius * radius;
    for (int32_t y = -radius; y <= radius; y++) {
        for (int32_t x = -radius; x <= radius; x++) {
            if (x * x + y * y <= r2)
                gx_surface_set(s, cx + x, cy + y, c);
        }
    }
}

void gx_fill_round_rect(gx_surface *s, gx_rect r, int32_t radius, gx_color c)
{
    if (gx_rect_empty(r))
        return;
    if (radius <= 0) {
        gx_fill_rect(s, r, c);
        return;
    }
    if (radius * 2 > r.w)
        radius = r.w / 2;
    if (radius * 2 > r.h)
        radius = r.h / 2;

    gx_fill_rect(s, gx_rect_make(r.x + radius, r.y, r.w - 2 * radius, r.h), c);
    gx_fill_rect(s, gx_rect_make(r.x, r.y + radius, radius, r.h - 2 * radius), c);
    gx_fill_rect(s, gx_rect_make(r.x + r.w - radius, r.y + radius, radius, r.h - 2 * radius), c);

    gx_fill_circle(s, r.x + radius, r.y + radius, radius, c);
    gx_fill_circle(s, r.x + r.w - radius - 1, r.y + radius, radius, c);
    gx_fill_circle(s, r.x + radius, r.y + r.h - radius - 1, radius, c);
    gx_fill_circle(s, r.x + r.w - radius - 1, r.y + r.h - radius - 1, radius, c);
}

void gx_blit(gx_surface *dst, int32_t dx, int32_t dy, const gx_surface *src)
{
    if (!dst || !src)
        return;
    gx_blit_rect(dst, dx, dy, src, gx_rect_make(0, 0, (int32_t)src->width, (int32_t)src->height));
}

void gx_blit_rect(gx_surface *dst, int32_t dx, int32_t dy,
                  const gx_surface *src, gx_rect src_r)
{
    if (!dst || !src || gx_rect_empty(src_r))
        return;

    for (int32_t y = 0; y < src_r.h; y++) {
        int32_t sy = src_r.y + y;
        int32_t dy2 = dy + y;
        if (sy < 0 || (uint32_t)sy >= src->height || dy2 < 0 || (uint32_t)dy2 >= dst->height)
            continue;
        for (int32_t x = 0; x < src_r.w; x++) {
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

void gx_blit_alpha(gx_surface *dst, int32_t dx, int32_t dy,
                   const gx_surface *src, uint8_t opacity)
{
    if (!dst || !src || opacity == 0)
        return;
    if (opacity == 255) {
        gx_blit(dst, dx, dy, src);
        return;
    }

    for (uint32_t y = 0; y < src->height; y++) {
        int32_t dy2 = dy + (int32_t)y;
        if (dy2 < 0 || (uint32_t)dy2 >= dst->height)
            continue;
        for (uint32_t x = 0; x < src->width; x++) {
            int32_t dx2 = dx + (int32_t)x;
            if (dx2 < 0 || (uint32_t)dx2 >= dst->width)
                continue;
            gx_color c = gx_color_mul_alpha(src->pixels[y * src->stride + x], opacity);
            gx_color *dp = &dst->pixels[(uint32_t)dy2 * dst->stride + (uint32_t)dx2];
            *dp = gx_blend(*dp, c);
        }
    }
}

void gx_blit_tint(gx_surface *dst, int32_t dx, int32_t dy,
                  const gx_surface *src, gx_color tint)
{
    if (!dst || !src)
        return;
    uint32_t ta = GX_A(tint);
    for (uint32_t y = 0; y < src->height; y++) {
        int32_t dy2 = dy + (int32_t)y;
        if (dy2 < 0 || (uint32_t)dy2 >= dst->height)
            continue;
        for (uint32_t x = 0; x < src->width; x++) {
            int32_t dx2 = dx + (int32_t)x;
            if (dx2 < 0 || (uint32_t)dx2 >= dst->width)
                continue;
            gx_color s = src->pixels[y * src->stride + x];
            gx_color mixed = GX_RGBA(
                (GX_R(s) * (255 - ta) + GX_R(tint) * ta) / 255,
                (GX_G(s) * (255 - ta) + GX_G(tint) * ta) / 255,
                (GX_B(s) * (255 - ta) + GX_B(tint) * ta) / 255,
                255);
            dst->pixels[(uint32_t)dy2 * dst->stride + (uint32_t)dx2] = mixed;
        }
    }
}

static gx_color lerp_color(gx_color a, gx_color b, uint32_t t, uint32_t max)
{
    if (max == 0)
        return a;
    uint32_t r = (GX_R(a) * (max - t) + GX_R(b) * t) / max;
    uint32_t g = (GX_G(a) * (max - t) + GX_G(b) * t) / max;
    uint32_t bl = (GX_B(a) * (max - t) + GX_B(b) * t) / max;
    uint32_t al = (GX_A(a) * (max - t) + GX_A(b) * t) / max;
    return GX_RGBA(r, g, bl, al);
}

void gx_gradient_v(gx_surface *s, gx_rect r, gx_color top, gx_color bottom)
{
    if (!s || gx_rect_empty(r))
        return;
    for (int32_t y = 0; y < r.h; y++) {
        gx_color c = lerp_color(top, bottom, (uint32_t)y, (uint32_t)(r.h > 1 ? r.h - 1 : 1));
        gx_fill_rect(s, gx_rect_make(r.x, r.y + y, r.w, 1), c);
    }
}

void gx_gradient_h(gx_surface *s, gx_rect r, gx_color left, gx_color right)
{
    if (!s || gx_rect_empty(r))
        return;
    for (int32_t x = 0; x < r.w; x++) {
        gx_color c = lerp_color(left, right, (uint32_t)x, (uint32_t)(r.w > 1 ? r.w - 1 : 1));
        gx_fill_rect(s, gx_rect_make(r.x + x, r.y, 1, r.h), c);
    }
}
