#include "accel.h"
#include "blur.h"
#include <kernel/string.h>
#include <drivers/ps2.h>
#include "compositor.h"
#include "server.h"

static int alloc_slot(gx_compositor *c)
{
    for (int i = 0; i < GX_MAX_LAYERS; i++) {
        if (!c->layers[i].used)
            return i;
    }
    return -1;
}

static void sort_ids_by_z(gx_compositor *c, int *ids, int *n)
{
    *n = 0;
    for (int i = 0; i < GX_MAX_LAYERS; i++) {
        if (c->layers[i].used && c->layers[i].visible)
            ids[(*n)++] = i;
    }
    for (int i = 0; i < *n; i++) {
        for (int j = i + 1; j < *n; j++) {
            if (c->layers[ids[j]].z < c->layers[ids[i]].z) {
                int t = ids[i];
                ids[i] = ids[j];
                ids[j] = t;
            }
        }
    }
}

/* Horizontal span inside rounded rect for row y (local). Matches gx_point_in_round_rect. */
static void round_span(int32_t y, int32_t w, int32_t h, int32_t r,
                       int32_t *x0, int32_t *x1)
{
    if (r <= 0) {
        *x0 = 0;
        *x1 = w;
        return;
    }
    if (r * 2 > w)
        r = w / 2;
    if (r * 2 > h)
        r = h / 2;
    if (r <= 0) {
        *x0 = 0;
        *x1 = w;
        return;
    }

    int32_t ly = y;
    if (ly >= h - r)
        ly = h - 1 - ly;

    if (ly >= r) {
        *x0 = 0;
        *x1 = w;
        return;
    }

    /* (r - x)^2 + (r - ly)^2 <= r^2  =>  (r - x)^2 <= r^2 - (r - ly)^2 */
    int32_t t = r - ly;
    int32_t max_d2 = r * r - t * t;
    int32_t dx = 0;
    while ((dx + 1) * (dx + 1) <= max_d2)
        dx++;

    *x0 = r - dx;
    *x1 = w - (r - dx);
    if (*x0 < 0)
        *x0 = 0;
    if (*x1 > w)
        *x1 = w;
}

int gx_compositor_init(gx_compositor *c, gx_device *dev)
{
    if (!c || !dev || !dev->ready)
        return -1;
    memset(c, 0, sizeof(*c));
    c->device = dev;
    return 0;
}

void gx_compositor_shutdown(gx_compositor *c)
{
    if (!c)
        return;
    if (c->wallpaper_blurred) {
        gx_surface_destroy(c->wallpaper_blurred);
        c->wallpaper_blurred = NULL;
    }
    c->wallpaper = NULL;
    memset(c->layers, 0, sizeof(c->layers));
}

void gx_compositor_set_wallpaper(gx_compositor *c, gx_surface *wp)
{
    if (!c)
        return;
    c->wallpaper = wp;

    if (c->wallpaper_blurred) {
        gx_surface_destroy(c->wallpaper_blurred);
        c->wallpaper_blurred = NULL;
    }
    if (!wp)
        return;

    /* Solid 1x1 wallpaper: compose fills the color; blur would just waste RAM. */
    if (wp->width <= 1 || wp->height <= 1)
        return;

    /* Pre-blur once so thin shell chrome can sample a stable frosted backdrop. */
    c->wallpaper_blurred = gx_surface_create(wp->width, wp->height);
    if (!c->wallpaper_blurred)
        return;
    if (gx_blur_copy(wp,
                     gx_rect_make(0, 0, (int32_t)wp->width, (int32_t)wp->height),
                     c->wallpaper_blurred, 10) < 0) {
        gx_surface_destroy(c->wallpaper_blurred);
        c->wallpaper_blurred = NULL;
    }
}

int gx_compositor_add_layer(gx_compositor *c, gx_layer *desc)
{
    if (!c || !desc)
        return -1;
    int id = alloc_slot(c);
    if (id < 0)
        return -1;

    c->layers[id] = *desc;
    c->layers[id].used = 1;
    if (c->layers[id].opacity == 0 && desc->style != GX_LAYER_OPAQUE)
        c->layers[id].opacity = 255;
    if (!c->layers[id].visible)
        c->layers[id].visible = 1;
    c->layer_count++;
    return id;
}

void gx_compositor_remove_layer(gx_compositor *c, int id)
{
    if (!c || id < 0 || id >= GX_MAX_LAYERS || !c->layers[id].used)
        return;
    memset(&c->layers[id], 0, sizeof(c->layers[id]));
    c->layer_count--;
}

gx_layer *gx_compositor_layer(gx_compositor *c, int id)
{
    if (!c || id < 0 || id >= GX_MAX_LAYERS || !c->layers[id].used)
        return NULL;
    return &c->layers[id];
}

void gx_compositor_raise(gx_compositor *c, int id)
{
    gx_layer *L = gx_compositor_layer(c, id);
    if (!L)
        return;
    int maxz = L->z;
    for (int i = 0; i < GX_MAX_LAYERS; i++) {
        if (c->layers[i].used && c->layers[i].z > maxz)
            maxz = c->layers[i].z;
    }
    L->z = maxz + 1;
}

static void blit_span(gx_surface *bb, int32_t dx, int32_t dy,
                      const gx_surface *src, int32_t sy,
                      int32_t sx0, int32_t sx1, uint8_t opacity)
{
    if (dy < 0 || (uint32_t)dy >= bb->height || sy < 0 || (uint32_t)sy >= src->height)
        return;
    if (sx0 < 0)
        sx0 = 0;
    if (sx1 > (int32_t)src->width)
        sx1 = (int32_t)src->width;

    for (int32_t sx = sx0; sx < sx1; sx++) {
        int32_t x = dx + sx;
        if (x < 0 || (uint32_t)x >= bb->width)
            continue;
        gx_color c = src->pixels[(uint32_t)sy * src->stride + (uint32_t)sx];
        if (opacity != 255)
            c = gx_color_mul_alpha(c, opacity);
        if (GX_A(c) == 0)
            continue;
        gx_color *dp = &bb->pixels[(uint32_t)dy * bb->stride + (uint32_t)x];
        if (GX_A(c) == 255)
            *dp = c;
        else
            *dp = gx_blend(*dp, c);
    }
}

/* Rounded-corner row: coverage-modulated blend for soft silhouette edges. */
static void blit_round_row(gx_surface *bb, int32_t dx, int32_t dy,
                           const gx_surface *src, int32_t sy, int32_t local_y,
                           int32_t w, int32_t h, int32_t rad, uint8_t opacity)
{
    if (dy < 0 || (uint32_t)dy >= bb->height || sy < 0 || (uint32_t)sy >= src->height)
        return;

    int32_t x0, x1;
    round_span(local_y, w, h, rad, &x0, &x1);
    /* Expand one pixel for AA fringe outside the hard span. */
    if (x0 > 0)
        x0--;
    if (x1 < w)
        x1++;

    for (int32_t sx = x0; sx < x1; sx++) {
        if (sx < 0 || sx >= (int32_t)src->width)
            continue;
        uint8_t cov = gx_round_coverage(sx, local_y, w, h, rad);
        if (cov == 0)
            continue;
        int32_t x = dx + sx;
        if (x < 0 || (uint32_t)x >= bb->width)
            continue;
        gx_color c = src->pixels[(uint32_t)sy * src->stride + (uint32_t)sx];
        if (opacity != 255)
            c = gx_color_mul_alpha(c, opacity);
        if (cov < 255)
            c = gx_color_mul_alpha(c, cov);
        if (GX_A(c) == 0)
            continue;
        gx_color *dp = &bb->pixels[(uint32_t)dy * bb->stride + (uint32_t)x];
        if (GX_A(c) == 255)
            *dp = c;
        else
            *dp = gx_blend(*dp, c);
    }
}

/* Fast path: opaque solid rows via memcpy when fully opaque source. */
static void blit_layer(gx_surface *bb, gx_layer *L, uint8_t opacity)
{
    gx_surface *src = L->surface;
    if (!src)
        return;

    gx_rect r = L->bounds;
    int32_t rad = L->corner_radius;
    int fast_opaque = (L->style == GX_LAYER_OPAQUE && opacity == 255 && rad <= 0);

    for (int32_t y = 0; y < r.h; y++) {
        int32_t sy = r.y + y;
        if (sy < 0 || (uint32_t)sy >= bb->height || (uint32_t)y >= src->height)
            continue;

        if ((y & 31) == 0)
            ps2_poll();

        if (rad > 0) {
            blit_round_row(bb, r.x, sy, src, y, y, r.w, r.h, rad, opacity);
            continue;
        }

        int32_t x0 = 0;
        int32_t x1 = r.w;
        if (x0 >= x1)
            continue;

        if (fast_opaque) {
            int32_t dx = r.x + x0;
            int32_t w = x1 - x0;
            if (dx < 0) {
                x0 -= dx;
                w += dx;
                dx = 0;
            }
            if (dx + w > (int32_t)bb->width)
                w = (int32_t)bb->width - dx;
            if (w > 0) {
                memcpy(&bb->pixels[(uint32_t)sy * bb->stride + (uint32_t)dx],
                       &src->pixels[(uint32_t)y * src->stride + (uint32_t)x0],
                       (size_t)w * sizeof(gx_color));
            }
        } else {
            blit_span(bb, r.x, sy, src, y, x0, x1, opacity);
        }
    }
}

static void paint_acrylic(gx_compositor *c, gx_surface *bb, gx_layer *L)
{
    gx_surface *blurred = c->wallpaper_blurred ? c->wallpaper_blurred : c->wallpaper;
    gx_rect r = L->bounds;
    int32_t rad = L->corner_radius;
    gx_color tint = L->tint;

    if (blurred) {
        for (int32_t y = 0; y < r.h; y++) {
            int32_t sy = r.y + y;
            if (sy < 0 || (uint32_t)sy >= bb->height)
                continue;

            /* Keep PS/2 drained during per-pixel acrylic work. */
            if ((y & 15) == 0)
                ps2_poll();

            int32_t x0, x1;
            if (rad > 0) {
                round_span(y, r.w, r.h, rad, &x0, &x1);
                if (x0 > 0)
                    x0--;
                if (x1 < r.w)
                    x1++;
            } else {
                x0 = 0;
                x1 = r.w;
            }
            for (int32_t x = x0; x < x1; x++) {
                int32_t sx = r.x + x;
                if (sx < 0 || (uint32_t)sx >= bb->width)
                    continue;
                uint8_t cov = rad > 0 ? gx_round_coverage(x, y, r.w, r.h, rad) : 255;
                if (cov == 0)
                    continue;
                gx_color base = GX_BLACK;
                if (blurred->width == 1 && blurred->height == 1)
                    base = blurred->pixels[0];
                else if ((uint32_t)sx < blurred->width &&
                         (uint32_t)sy < blurred->height)
                    base = blurred->pixels[(uint32_t)sy * blurred->stride +
                                           (uint32_t)sx];
                gx_color t = tint;
                if (cov < 255)
                    t = gx_color_mul_alpha(t, cov);
                gx_color *dp = &bb->pixels[(uint32_t)sy * bb->stride + (uint32_t)sx];
                *dp = gx_blend(base, t);
            }
        }
    }

    blit_layer(bb, L, L->opacity ? L->opacity : 255);
}

static void paint_blur_behind(gx_compositor *c, gx_surface *bb, gx_layer *L)
{
    /* Live per-frame blur: sample pre-blurred wallpaper (set once). */
    paint_acrylic(c, bb, L);
}

void gx_compositor_compose(gx_compositor *c)
{
    if (!c || !c->device || !c->device->backbuffer)
        return;

    gx_surface *bb = c->device->backbuffer;

    if (c->wallpaper) {
        if (c->wallpaper->width == 1 && c->wallpaper->height == 1) {
            gx_surface_clear(bb, c->wallpaper->pixels[0]);
        } else if (c->wallpaper->width == bb->width &&
                   c->wallpaper->height == bb->height &&
                   c->wallpaper->stride == bb->stride) {
            memcpy(bb->pixels, c->wallpaper->pixels,
                   (size_t)bb->stride * bb->height * sizeof(gx_color));
        } else {
            gx_accel_blit(bb, 0, 0, c->wallpaper);
        }
    } else {
        gx_surface_clear(bb, GX_RGB(20, 22, 30));
    }

    int ids[GX_MAX_LAYERS];
    int n = 0;
    sort_ids_by_z(c, ids, &n);

    for (int i = 0; i < n; i++) {
        gx_layer *L = &c->layers[ids[i]];
        /* Drain mouse/keyboard between layers so compose doesn't starve input. */
        gx_server_poll_input();
        switch (L->style) {
        case GX_LAYER_ACRYLIC:
            paint_acrylic(c, bb, L);
            break;
        case GX_LAYER_BLUR_BEHIND:
            paint_blur_behind(c, bb, L);
            break;
        case GX_LAYER_ALPHA:
            blit_layer(bb, L, L->opacity ? L->opacity : 255);
            break;
        case GX_LAYER_OPAQUE:
        default:
            blit_layer(bb, L, 255);
            break;
        }
    }
}
