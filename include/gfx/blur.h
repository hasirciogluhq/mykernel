#ifndef MYKERNEL_GFX_BLUR_H
#define MYKERNEL_GFX_BLUR_H

#include <gfx/surface.h>

/* Separable box blur. radius 1..16. Allocates temporary row/col buffers. */
int  gx_blur_box(gx_surface *s, int radius);
int  gx_blur_box_rect(gx_surface *s, gx_rect r, int radius);

/* Copy rect from src, blur into dst (same size as rect). */
int  gx_blur_copy(const gx_surface *src, gx_rect r, gx_surface *dst, int radius);

#endif
