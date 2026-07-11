#ifndef MYKERNEL_GFX_DRAW_H
#define MYKERNEL_GFX_DRAW_H

#include <gfx/surface.h>

void gx_fill_rect(gx_surface *s, gx_rect r, gx_color c);
void gx_fill_rect_aa(gx_surface *s, gx_rect r, gx_color c); /* alpha blend */
void gx_draw_rect(gx_surface *s, gx_rect r, gx_color c, int thickness);
void gx_draw_line(gx_surface *s, int32_t x0, int32_t y0, int32_t x1, int32_t y1, gx_color c);
void gx_fill_circle(gx_surface *s, int32_t cx, int32_t cy, int32_t radius, gx_color c);
void gx_fill_round_rect(gx_surface *s, gx_rect r, int32_t radius, gx_color c);
void gx_blit(gx_surface *dst, int32_t dx, int32_t dy, const gx_surface *src);
void gx_blit_rect(gx_surface *dst, int32_t dx, int32_t dy,
                  const gx_surface *src, gx_rect src_r);
void gx_blit_alpha(gx_surface *dst, int32_t dx, int32_t dy,
                   const gx_surface *src, uint8_t opacity);
void gx_blit_tint(gx_surface *dst, int32_t dx, int32_t dy,
                  const gx_surface *src, gx_color tint);
void gx_gradient_v(gx_surface *s, gx_rect r, gx_color top, gx_color bottom);
void gx_gradient_h(gx_surface *s, gx_rect r, gx_color left, gx_color right);

#endif
