#ifndef MYKERNEL_GFX_COMPOSITOR_H
#define MYKERNEL_GFX_COMPOSITOR_H

#include <gfx/device.h>
#include <gfx/surface.h>

#define GX_MAX_LAYERS 32

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

typedef struct gx_compositor {
    gx_device   *device;
    gx_surface *wallpaper;
    gx_surface *wallpaper_blurred;
    gx_layer    layers[GX_MAX_LAYERS];
    int         layer_count;
} gx_compositor;

int  gx_compositor_init(gx_compositor *c, gx_device *dev);
void gx_compositor_shutdown(gx_compositor *c);
void gx_compositor_set_wallpaper(gx_compositor *c, gx_surface *wp);
int  gx_compositor_add_layer(gx_compositor *c, gx_layer *desc);
void gx_compositor_remove_layer(gx_compositor *c, int id);
gx_layer *gx_compositor_layer(gx_compositor *c, int id);
void gx_compositor_raise(gx_compositor *c, int id);
void gx_compositor_compose(gx_compositor *c);

#endif
