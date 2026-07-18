#ifndef MYKERNEL_MKDX_H
#define MYKERNEL_MKDX_H

/*
 * MKDX — Direct eXperience graphics for hsrc-kernel (loadable driver API)
 * Compositor / WM / accel live in mkdx.kmod.
 * UI is drawn by userspace via <user/gx.h>.
 */

#include "color.h"
#include "surface.h"
#include "draw.h"
#include "blur.h"
#include "font.h"
#include "device.h"
#include "accel.h"
#include "compositor.h"
#include "window.h"
#include "server.h"
#include "context.h"
#include "render3d.h"

/* Public driver exports */
int mkdx_get_screen_size(uint32_t *w, uint32_t *h, uint32_t *bpp);
int mkdx_present(void);

#endif
