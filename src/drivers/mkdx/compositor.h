#ifndef MYKERNEL_GFX_COMPOSITOR_H
#define MYKERNEL_GFX_COMPOSITOR_H

#include "device.h"
#include "surface.h"

#define GX_MAX_LAYERS 40

typedef enum {
    GX_LAYER_OPAQUE = 0,
    GX_LAYER_ALPHA,
    GX_LAYER_ACRYLIC,   /* blur wallpaper + tint */
    GX_LAYER_BLUR_BEHIND
} gx_layer_style;

typedef struct gx_layer {
    int         used;
    int         visible;
    int         z;
    gx_rect     bounds;
    gx_surface *surface;
    gx_layer_style style;
    uint8_t     opacity;   /* 0-255 */
    gx_color    tint;      /* for acrylic */
    int         blur_radius;
    int32_t     corner_radius; /* 0 = sharp */
} gx_layer;

/* Cached frosted+tint backdrop for acrylic layers (dock/menubar). */
#define GX_ACRYLIC_CACHE_MAX 4

typedef struct gx_acrylic_cache {
    int         used;
    int         ready;
    int         layer_id;
    gx_rect     bounds;
    gx_color    tint;
    int32_t     radius;
    uint32_t    wp_gen;
    gx_surface *tile;
} gx_acrylic_cache;

typedef struct gx_compositor {
    gx_device   *device;
    gx_surface *wallpaper;
    gx_surface *wallpaper_blurred;
    uint32_t    wallpaper_gen; /* bumps on wallpaper change → drop acrylic tiles */
    gx_layer    layers[GX_MAX_LAYERS];
    int         layer_count;
    gx_acrylic_cache acrylic_cache[GX_ACRYLIC_CACHE_MAX];
} gx_compositor;

int  gx_compositor_init(gx_compositor *c, gx_device *dev);
void gx_compositor_shutdown(gx_compositor *c);
void gx_compositor_set_wallpaper(gx_compositor *c, gx_surface *wp);
int  gx_compositor_add_layer(gx_compositor *c, gx_layer *desc);
void gx_compositor_remove_layer(gx_compositor *c, int id);
gx_layer *gx_compositor_layer(gx_compositor *c, int id);
void gx_compositor_raise(gx_compositor *c, int id);
/* Live drag: opaque slide (no acrylic rebuild); corner mask kept. -1 = off. */
void gx_compositor_set_drag_layer(int layer_id);
/* Sprite drag: save underlay, restore old, capture new, blit window. */
void gx_compositor_drag_slide(gx_compositor *c, gx_surface *dst,
                              gx_rect old_r, gx_rect new_r, int layer_id);
void gx_compositor_drag_end(void);
void gx_compositor_compose(gx_compositor *c);
/* Compose only inside clip into dst (cursor-free scene). clip empty → no-op. */
void gx_compositor_compose_rect(gx_compositor *c, gx_surface *dst, gx_rect clip);

#endif
