#include <gfx/blur.h>
#include <kernel/heap.h>

static void blur_h(gx_color *dst, const gx_color *src, uint32_t w, uint32_t h,
                   uint32_t stride, int radius)
{
    int diam = radius * 2 + 1;
    for (uint32_t y = 0; y < h; y++) {
        const gx_color *srow = src + y * stride;
        gx_color *drow = dst + y * stride;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t r = 0, g = 0, b = 0, a = 0;
            int count = 0;
            for (int k = -radius; k <= radius; k++) {
                int xi = (int)x + k;
                if (xi < 0 || xi >= (int)w)
                    continue;
                gx_color c = srow[xi];
                r += GX_R(c);
                g += GX_G(c);
                b += GX_B(c);
                a += GX_A(c);
                count++;
            }
            if (count == 0)
                count = diam;
            drow[x] = GX_RGBA(r / count, g / count, b / count, a / count);
        }
    }
}

static void blur_v(gx_color *dst, const gx_color *src, uint32_t w, uint32_t h,
                   uint32_t stride, int radius)
{
    int diam = radius * 2 + 1;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t r = 0, g = 0, b = 0, a = 0;
            int count = 0;
            for (int k = -radius; k <= radius; k++) {
                int yi = (int)y + k;
                if (yi < 0 || yi >= (int)h)
                    continue;
                gx_color c = src[(uint32_t)yi * stride + x];
                r += GX_R(c);
                g += GX_G(c);
                b += GX_B(c);
                a += GX_A(c);
                count++;
            }
            if (count == 0)
                count = diam;
            dst[y * stride + x] = GX_RGBA(r / count, g / count, b / count, a / count);
        }
    }
}

int gx_blur_box(gx_surface *s, int radius)
{
    return gx_blur_box_rect(s, gx_rect_make(0, 0, (int32_t)s->width, (int32_t)s->height), radius);
}

int gx_blur_box_rect(gx_surface *s, gx_rect r, int radius)
{
    if (!s || gx_rect_empty(r) || radius < 1)
        return -1;
    if (radius > 16)
        radius = 16;

    gx_rect clip = gx_rect_intersect(r, gx_rect_make(0, 0, (int32_t)s->width, (int32_t)s->height));
    if (gx_rect_empty(clip))
        return -1;

    uint32_t w = (uint32_t)clip.w;
    uint32_t h = (uint32_t)clip.h;
    size_t bytes = (size_t)w * h * sizeof(gx_color);
    gx_color *tmp = kmalloc(bytes);
    gx_color *tmp2 = kmalloc(bytes);
    if (!tmp || !tmp2) {
        if (tmp)
            kfree(tmp);
        if (tmp2)
            kfree(tmp2);
        return -1;
    }

    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            tmp[y * w + x] = s->pixels[(uint32_t)(clip.y + (int32_t)y) * s->stride +
                                       (uint32_t)(clip.x + (int32_t)x)];
        }
    }

    blur_h(tmp2, tmp, w, h, w, radius);
    blur_v(tmp, tmp2, w, h, w, radius);

    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            s->pixels[(uint32_t)(clip.y + (int32_t)y) * s->stride +
                      (uint32_t)(clip.x + (int32_t)x)] = tmp[y * w + x];
        }
    }

    kfree(tmp2);
    kfree(tmp);
    return 0;
}

int gx_blur_copy(const gx_surface *src, gx_rect r, gx_surface *dst, int radius)
{
    if (!src || !dst || gx_rect_empty(r))
        return -1;
    if ((uint32_t)r.w != dst->width || (uint32_t)r.h != dst->height)
        return -1;

    for (int32_t y = 0; y < r.h; y++) {
        for (int32_t x = 0; x < r.w; x++) {
            int32_t sx = r.x + x;
            int32_t sy = r.y + y;
            gx_color c = GX_BLACK;
            if (sx >= 0 && sy >= 0 && (uint32_t)sx < src->width && (uint32_t)sy < src->height)
                c = src->pixels[(uint32_t)sy * src->stride + (uint32_t)sx];
            dst->pixels[(uint32_t)y * dst->stride + (uint32_t)x] = c;
        }
    }
    return gx_blur_box(dst, radius);
}
