#include <gfx/device.h>
#include <kernel/heap.h>

int gx_device_init(gx_device *dev)
{
    if (!dev)
        return -1;

    dev->fb = fb_get();
    if (!dev->fb || !dev->fb->ready)
        return -1;

    dev->backbuffer = gx_surface_create(dev->fb->width, dev->fb->height);
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
    dev->ready = 0;
}

gx_surface *gx_device_target(gx_device *dev)
{
    return dev ? dev->backbuffer : NULL;
}

void gx_device_present(gx_device *dev)
{
    if (!dev || !dev->ready || !dev->backbuffer)
        return;
    fb_present(dev->backbuffer->pixels, dev->backbuffer->stride);
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
