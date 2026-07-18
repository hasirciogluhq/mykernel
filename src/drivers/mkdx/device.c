#include "device.h"
#include <kernel/heap.h>

int gx_device_init(gx_device *dev)
{
    display_ops_t *ops;
    display_mode_t mode;

    if (!dev)
        return -1;

    ops = display_active();
    if (!ops || !ops->get_mode)
        return -1;
    if (ops->get_mode(&mode) < 0)
        return -1;

    dev->mode = mode;
    dev->framebuffer = (uint32_t *)mode.addr;
    dev->backbuffer = gx_surface_create(mode.width, mode.height);
    if (!dev->backbuffer)
        return -1;

    dev->ready = 1;
    return 0;
}

void gx_device_shutdown(gx_device *dev)
{
    if (!dev)
        return;
    if (dev->backbuffer) {
        gx_surface_destroy(dev->backbuffer);
        dev->backbuffer = NULL;
    }
    dev->framebuffer = NULL;
    dev->ready = 0;
}

gx_surface *gx_device_target(gx_device *dev)
{
    return dev ? dev->backbuffer : NULL;
}

uint32_t *gx_device_framebuffer(gx_device *dev)
{
    return (dev && dev->ready) ? dev->framebuffer : NULL;
}

void gx_device_present(gx_device *dev)
{
    display_ops_t *ops;
    display_mode_t mode;

    if (!dev || !dev->ready || !dev->backbuffer)
        return;

    /* Prefer active display path: virtio-gpu (higher prio) else BGA. */
    ops = display_active();
    if (!ops || !ops->present)
        return;

    /* Refresh framebuffer pointer in case the driver rebound scanout. */
    if (ops->get_mode && ops->get_mode(&mode) == 0) {
        dev->mode = mode;
        dev->framebuffer = (uint32_t *)mode.addr;
    }

    ops->present(dev->backbuffer->pixels, dev->backbuffer->stride);
}

void gx_device_begin(gx_device *dev, gx_color clear)
{
    if (!dev || !dev->backbuffer)
        return;
    gx_surface_clear(dev->backbuffer, clear);
}

void gx_device_end(gx_device *dev)
{
    gx_device_present(dev);
}
