#ifndef MYKERNEL_GFX_DEVICE_H
#define MYKERNEL_GFX_DEVICE_H

#include <gfx/surface.h>
#include <drivers/fb.h>

/* MKDX device — software Direct2D-style render target backed by FB. */
typedef struct gx_device {
    gx_surface *backbuffer;
    const fb_info_t *fb;
    int ready;
} gx_device;

int        gx_device_init(gx_device *dev);
void       gx_device_shutdown(gx_device *dev);
gx_surface *gx_device_target(gx_device *dev);
void       gx_device_present(gx_device *dev);
void       gx_device_begin(gx_device *dev, gx_color clear);
void       gx_device_end(gx_device *dev);

#endif
