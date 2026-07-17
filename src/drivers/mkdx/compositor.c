#include "accel.h"
#include "blur.h"
#include <kernel/heap.h>
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

static void acrylic_cache_clear(gx_compositor *c)
{
    int i;
    if (!c)
        return;
    for (i = 0; i < GX_ACRYLIC_CACHE_MAX; i++) {
        if (c->acrylic_cache[i].tile)
            gx_surface_destroy(c->acrylic_cache[i].tile);
        memset(&c->acrylic_cache[i], 0, sizeof(c->acrylic_cache[i]));
    }
}

void gx_compositor_shutdown(gx_compositor *c)
{
    if (!c)
        return;
    acrylic_cache_clear(c);
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
    c->wallpaper_gen++;
    acrylic_cache_clear(c);

    if (c->wallpaper_blurred) {
        gx_surface_destroy(c->wallpaper_blurred);
        c->wallpaper_blurred = NULL;
    }
    if (!wp)
        return;

    /* Solid 1x1 wallpaper: compose fills the color; blur would just waste RAM. */
    if (wp->width <= 1 || wp->height <= 1)
        return;

    /*
     * Pre-blur needs the output surface plus two full-size scratch buffers.
     * On the bump heap that peak often succeeds for the output then fails the
     * scratch alloc — or worse, succeeds and leaves no RAM for app windows.
     * Skip when free heap cannot hold output + 2× scratch + ~2MiB headroom.
     */
    {
        size_t bytes = (size_t)wp->width * (size_t)wp->height * sizeof(gx_color);
        size_t need = bytes * 3u + (2u * 1024u * 1024u);
        if (heap_free() < need)
            return;
    }

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

static gx_acrylic_cache *acrylic_cache_get(gx_compositor *c, int layer_id,
                                           gx_layer *L)
{
    int i;
    int free_slot = -1;
    gx_acrylic_cache *e;

    for (i = 0; i < GX_ACRYLIC_CACHE_MAX; i++) {
        e = &c->acrylic_cache[i];
        if (!e->used) {
            if (free_slot < 0)
                free_slot = i;
            continue;
        }
        if (e->layer_id == layer_id &&
            e->bounds.x == L->bounds.x && e->bounds.y == L->bounds.y &&
            e->bounds.w == L->bounds.w && e->bounds.h == L->bounds.h &&
            e->tint == L->tint && e->radius == L->corner_radius &&
            e->wp_gen == c->wallpaper_gen && e->tile)
            return e;
    }

    if (free_slot < 0)
        free_slot = layer_id & (GX_ACRYLIC_CACHE_MAX - 1);

    e = &c->acrylic_cache[free_slot];
    if (e->tile) {
        gx_surface_destroy(e->tile);
        e->tile = NULL;
    }
    memset(e, 0, sizeof(*e));
    e->layer_id = layer_id;
    e->bounds = L->bounds;
    e->tint = L->tint;
    e->radius = L->corner_radius;
    e->wp_gen = c->wallpaper_gen;
    e->ready = 0;
    e->tile = gx_surface_create((uint32_t)L->bounds.w, (uint32_t)L->bounds.h);
    if (!e->tile)
        return NULL;
    e->used = 1;
    return e;
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

/* Fast path: opaque solid rows via memcpy when fully opaque source.
 * clip is in screen space; empty clip means full layer bounds. */
static void blit_layer(gx_surface *bb, gx_layer *L, uint8_t opacity, gx_rect clip)
{
    gx_surface *src = L->surface;
    if (!src)
        return;

    gx_rect r = L->bounds;
    gx_rect vis = gx_rect_empty(clip) ? r : gx_rect_intersect(r, clip);
    if (gx_rect_empty(vis))
        return;

    int32_t rad = L->corner_radius;
    int fast_opaque = (L->style == GX_LAYER_OPAQUE && opacity == 255 && rad <= 0);
    int32_t y0 = vis.y - r.y;
    int32_t y1 = y0 + vis.h;

    for (int32_t y = y0; y < y1; y++) {
        int32_t sy = r.y + y;
        if (sy < 0 || (uint32_t)sy >= bb->height || (uint32_t)y >= src->height)
            continue;

        if ((y & 31) == 0)
            ps2_poll();

        if (rad > 0) {
            /* Restrict round-row scan to the clipped horizontal span. */
            int32_t lx0 = vis.x - r.x;
            int32_t lx1 = lx0 + vis.w;
            int32_t rx0, rx1;
            round_span(y, r.w, r.h, rad, &rx0, &rx1);
            if (rx0 > 0)
                rx0--;
            if (rx1 < r.w)
                rx1++;
            if (lx0 > rx0)
                rx0 = lx0;
            if (lx1 < rx1)
                rx1 = lx1;
            for (int32_t sx = rx0; sx < rx1; sx++) {
                if (sx < 0 || sx >= (int32_t)src->width)
                    continue;
                uint8_t cov = gx_round_coverage(sx, y, r.w, r.h, rad);
                if (cov == 0)
                    continue;
                int32_t x = r.x + sx;
                if (x < 0 || (uint32_t)x >= bb->width)
                    continue;
                gx_color c = src->pixels[(uint32_t)y * src->stride + (uint32_t)sx];
                if (opacity != 255)
                    c = gx_color_mul_alpha(c, opacity);
                if (cov < 255)
                    c = gx_color_mul_alpha(c, cov);
                if (GX_A(c) == 0)
                    continue;
                gx_color *dp = &bb->pixels[(uint32_t)sy * bb->stride + (uint32_t)x];
                if (GX_A(c) == 255)
                    *dp = c;
                else
                    *dp = gx_blend(*dp, c);
            }
            continue;
        }

        int32_t x0 = vis.x - r.x;
        int32_t x1 = x0 + vis.w;
        if (x0 < 0)
            x0 = 0;
        if (x1 > r.w)
            x1 = r.w;
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

static void paint_acrylic_tile(gx_compositor *c, gx_acrylic_cache *e, gx_layer *L)
{
    gx_surface *blurred = c->wallpaper_blurred ? c->wallpaper_blurred : c->wallpaper;
    gx_surface *tile = e->tile;
    gx_rect r = L->bounds;
    int32_t rad = L->corner_radius;
    gx_color tint = L->tint;
    int32_t y, x;

    if (!tile)
        return;
    gx_surface_clear(tile, GX_TRANSPARENT);
    if (!blurred)
        return;

    for (y = 0; y < r.h; y++) {
        int32_t sy = r.y + y;
        int32_t x0 = 0, x1 = r.w;
        if ((y & 15) == 0)
            ps2_poll();
        if (rad > 0) {
            int32_t rx0, rx1;
            round_span(y, r.w, r.h, rad, &rx0, &rx1);
            if (rx0 > 0)
                rx0--;
            if (rx1 < r.w)
                rx1++;
            x0 = rx0;
            x1 = rx1;
        }
        for (x = x0; x < x1; x++) {
            int32_t sx = r.x + x;
            uint8_t cov = rad > 0 ? gx_round_coverage(x, y, r.w, r.h, rad) : 255;
            gx_color base = GX_BLACK;
            gx_color t = tint;
            if (cov == 0)
                continue;
            if (blurred->width == 1 && blurred->height == 1)
                base = blurred->pixels[0];
            else if (sx >= 0 && sy >= 0 &&
                     (uint32_t)sx < blurred->width &&
                     (uint32_t)sy < blurred->height)
                base = blurred->pixels[(uint32_t)sy * blurred->stride +
                                       (uint32_t)sx];
            if (cov < 255)
                t = gx_color_mul_alpha(t, cov);
            tile->pixels[(uint32_t)y * tile->stride + (uint32_t)x] =
                gx_blend(base, t);
        }
    }
}

static void paint_acrylic(gx_compositor *c, gx_surface *bb, int layer_id,
                          gx_layer *L, gx_rect clip)
{
    gx_rect r = L->bounds;
    gx_rect vis = gx_rect_empty(clip) ? r : gx_rect_intersect(r, clip);
    gx_acrylic_cache *cache;
    int32_t y;

    if (gx_rect_empty(vis))
        return;

    cache = acrylic_cache_get(c, layer_id, L);
    if (cache && cache->tile) {
        int32_t x;
        if (!cache->ready) {
            paint_acrylic_tile(c, cache, L);
            cache->ready = 1;
        }

        /* Alpha-aware blit so rounded corners don't punch the wallpaper. */
        for (y = 0; y < vis.h; y++) {
            int32_t sy = vis.y + y;
            int32_t ty = vis.y - r.y + y;
            int32_t tx = vis.x - r.x;
            if (sy < 0 || (uint32_t)sy >= bb->height || ty < 0 ||
                (uint32_t)ty >= cache->tile->height)
                continue;
            for (x = 0; x < vis.w; x++) {
                gx_color src =
                    cache->tile->pixels[(uint32_t)ty * cache->tile->stride +
                                        (uint32_t)(tx + x)];
                gx_color *dp =
                    &bb->pixels[(uint32_t)sy * bb->stride + (uint32_t)(vis.x + x)];
                if (GX_A(src) == 0)
                    continue;
                if (GX_A(src) == 255)
                    *dp = src;
                else
                    *dp = gx_blend(*dp, src);
            }
        }
    } else {
        /* Fallback: direct sample (no tile memory). */
        gx_surface *blurred =
            c->wallpaper_blurred ? c->wallpaper_blurred : c->wallpaper;
        int32_t rad = L->corner_radius;
        gx_color tint = L->tint;
        int32_t y0 = vis.y - r.y;
        int32_t y1 = y0 + vis.h;
        for (int32_t ly = y0; ly < y1; ly++) {
            int32_t sy = r.y + ly;
            int32_t x0 = vis.x - r.x;
            int32_t x1 = x0 + vis.w;
            if (sy < 0 || (uint32_t)sy >= bb->height)
                continue;
            if ((ly & 15) == 0)
                ps2_poll();
            if (rad > 0) {
                int32_t rx0, rx1;
                round_span(ly, r.w, r.h, rad, &rx0, &rx1);
                if (rx0 > 0)
                    rx0--;
                if (rx1 < r.w)
                    rx1++;
                if (x0 < rx0)
                    x0 = rx0;
                if (x1 > rx1)
                    x1 = rx1;
            }
            for (int32_t lx = x0; lx < x1; lx++) {
                int32_t sx = r.x + lx;
                uint8_t cov =
                    rad > 0 ? gx_round_coverage(lx, ly, r.w, r.h, rad) : 255;
                gx_color base = GX_BLACK;
                gx_color t = tint;
                if (cov == 0 || sx < 0 || (uint32_t)sx >= bb->width)
                    continue;
                if (blurred) {
                    if (blurred->width == 1 && blurred->height == 1)
                        base = blurred->pixels[0];
                    else if ((uint32_t)sx < blurred->width &&
                             (uint32_t)sy < blurred->height)
                        base = blurred->pixels[(uint32_t)sy * blurred->stride +
                                               (uint32_t)sx];
                }
                if (cov < 255)
                    t = gx_color_mul_alpha(t, cov);
                bb->pixels[(uint32_t)sy * bb->stride + (uint32_t)sx] =
                    gx_blend(base, t);
            }
        }
    }

    blit_layer(bb, L, L->opacity ? L->opacity : 255, clip);
}

static void paint_wallpaper_rect(gx_compositor *c, gx_surface *dst, gx_rect clip)
{
    gx_color fallback = GX_RGB(20, 22, 30);

    if (!c->wallpaper) {
        gx_accel_fill(dst, clip, fallback);
        return;
    }

    if (c->wallpaper->width == 1 && c->wallpaper->height == 1) {
        gx_accel_fill(dst, clip, c->wallpaper->pixels[0]);
        return;
    }

    if (c->wallpaper->width == dst->width &&
        c->wallpaper->height == dst->height &&
        c->wallpaper->stride == dst->stride) {
        for (int32_t y = 0; y < clip.h; y++) {
            int32_t sy = clip.y + y;
            memcpy(&dst->pixels[(uint32_t)sy * dst->stride + (uint32_t)clip.x],
                   &c->wallpaper->pixels[(uint32_t)sy * c->wallpaper->stride +
                                         (uint32_t)clip.x],
                   (size_t)clip.w * sizeof(gx_color));
        }
        return;
    }

    gx_accel_blit_rect(dst, clip.x, clip.y, c->wallpaper, clip);
}

void gx_compositor_compose_rect(gx_compositor *c, gx_surface *dst, gx_rect clip)
{
    gx_rect screen;
    int ids[GX_MAX_LAYERS];
    int n = 0;
    int start = 0;
    int i;

    if (!c || !dst || !dst->pixels)
        return;

    screen = gx_rect_make(0, 0, (int32_t)dst->width, (int32_t)dst->height);
    clip = gx_rect_intersect(clip, screen);
    if (gx_rect_empty(clip))
        return;

    sort_ids_by_z(c, ids, &n);

    /*
     * Occlusion (P13): if a sharp opaque layer fully covers the clip, skip
     * wallpaper and everything below it — still correct for partial damage.
     * Rounded windows: use the axis-aligned inset (exclude corner radii) so
     * hover/content damage under opaque clients does not repaint wallpaper.
     */
    for (i = n - 1; i >= 0; i--) {
        gx_layer *L = &c->layers[ids[i]];
        int32_t ox, oy, ow, oh;
        int r;

        if (L->style != GX_LAYER_OPAQUE)
            continue;
        if (L->opacity && L->opacity < 255)
            continue;
        ox = L->bounds.x;
        oy = L->bounds.y;
        ow = L->bounds.w;
        oh = L->bounds.h;
        r = L->corner_radius;
        if (r > 0) {
            if (ow <= 2 * r || oh <= 2 * r)
                continue;
            ox += r;
            oy += r;
            ow -= 2 * r;
            oh -= 2 * r;
        }
        if (ox <= clip.x && oy <= clip.y &&
            ox + ow >= clip.x + clip.w &&
            oy + oh >= clip.y + clip.h) {
            start = i;
            break;
        }
    }

    if (start == 0)
        paint_wallpaper_rect(c, dst, clip);

    for (i = start; i < n; i++) {
        gx_layer *L = &c->layers[ids[i]];
        /* Drain mouse/keyboard between layers so compose doesn't starve input. */
        gx_server_poll_input();
        if (gx_rect_empty(gx_rect_intersect(L->bounds, clip)))
            continue;
        switch (L->style) {
        case GX_LAYER_ACRYLIC:
            paint_acrylic(c, dst, ids[i], L, clip);
            break;
        case GX_LAYER_BLUR_BEHIND:
            paint_acrylic(c, dst, ids[i], L, clip);
            break;
        case GX_LAYER_ALPHA:
            blit_layer(dst, L, L->opacity ? L->opacity : 255, clip);
            break;
        case GX_LAYER_OPAQUE:
        default:
            blit_layer(dst, L, 255, clip);
            break;
        }
    }
}

void gx_compositor_compose(gx_compositor *c)
{
    gx_surface *bb;
    if (!c || !c->device || !c->device->backbuffer)
        return;
    bb = c->device->backbuffer;
    gx_compositor_compose_rect(c, bb,
                               gx_rect_make(0, 0, (int32_t)bb->width, (int32_t)bb->height));
}
