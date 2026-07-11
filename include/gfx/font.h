#ifndef MYKERNEL_GFX_FONT_H
#define MYKERNEL_GFX_FONT_H

#include <gfx/surface.h>

#define GX_FONT_W 8
#define GX_FONT_H 8

void gx_draw_char(gx_surface *s, int32_t x, int32_t y, uint8_t ch, gx_color c);
void gx_draw_text(gx_surface *s, int32_t x, int32_t y, const char *text, gx_color c);
int  gx_text_width(const char *text);
int  gx_text_height(void);

#endif
