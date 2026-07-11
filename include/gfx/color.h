#ifndef MYKERNEL_GFX_COLOR_H
#define MYKERNEL_GFX_COLOR_H

#include <kernel/types.h>

/* ARGB8888 — alpha in high byte */
typedef uint32_t gx_color;

#define GX_A(c) (((c) >> 24) & 0xFFu)
#define GX_R(c) (((c) >> 16) & 0xFFu)
#define GX_G(c) (((c) >> 8) & 0xFFu)
#define GX_B(c) ((c) & 0xFFu)

#define GX_RGBA(r, g, b, a) \
    ((((gx_color)(a) & 0xFFu) << 24) | \
     (((gx_color)(r) & 0xFFu) << 16) | \
     (((gx_color)(g) & 0xFFu) << 8)  | \
     (((gx_color)(b) & 0xFFu)))

#define GX_RGB(r, g, b) GX_RGBA(r, g, b, 255)

#define GX_TRANSPARENT  GX_RGBA(0, 0, 0, 0)
#define GX_BLACK        GX_RGB(0, 0, 0)
#define GX_WHITE        GX_RGB(255, 255, 255)
#define GX_GRAY(n)      GX_RGB(n, n, n)

static inline gx_color gx_color_mul_alpha(gx_color c, uint8_t a)
{
    uint32_t na = (GX_A(c) * a) / 255u;
    return (c & 0x00FFFFFFu) | (na << 24);
}

static inline gx_color gx_blend(gx_color dst, gx_color src)
{
    uint32_t sa = GX_A(src);
    if (sa == 0)
        return dst;
    if (sa == 255)
        return src;

    uint32_t inv = 255u - sa;
    uint32_t r = (GX_R(src) * sa + GX_R(dst) * inv) / 255u;
    uint32_t g = (GX_G(src) * sa + GX_G(dst) * inv) / 255u;
    uint32_t b = (GX_B(src) * sa + GX_B(dst) * inv) / 255u;
    uint32_t a = sa + (GX_A(dst) * inv) / 255u;
    return GX_RGBA(r, g, b, a);
}

#endif
