#ifndef MYKERNEL_MKDX_DEVICE_H
#define MYKERNEL_MKDX_DEVICE_H

#include "surface.h"
#include <drivers/display.h>

/*
 * MKDX scanout device — double-buffered:
 *   backbuffer  = compose / draw target (CPU surface)
 *   framebuffer = display LFB / GPU scanout resource (mode.addr)
 * Present copies or page-flips backbuffer → framebuffer via display_ops
 * (virtio-gpu when registered, else BGA LFB flip/copy).
 */
typedef struct gx_device {
    gx_surface     *backbuffer;
    uint32_t       *framebuffer; /* scanout pixels (display mode.addr) */
    display_mode_t  mode;
    int             ready;
} gx_device;

int         gx_device_init(gx_device *dev);
void        gx_device_shutdown(gx_device *dev);
gx_surface *gx_device_target(gx_device *dev);
uint32_t   *gx_device_framebuffer(gx_device *dev);
void        gx_device_present(gx_device *dev);
void        gx_device_begin(gx_device *dev, gx_color clear);
void        gx_device_end(gx_device *dev);

#endif
