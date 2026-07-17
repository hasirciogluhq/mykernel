#ifndef MYKERNEL_MKDX_SERVER_H
#define MYKERNEL_MKDX_SERVER_H

#include "device.h"
#include "compositor.h"
#include "window.h"

typedef struct gx_server {
    gx_device      device;
    gx_compositor comp;
    wm_t          wm;
    gx_surface   *wallpaper;
    gx_surface   *cursor;      /* ARGB cursor glyph */
    int           dirty;       /* 1 = needs compose/present */
    int           dirty_full;  /* 1 = full-screen damage */
    gx_rect       dirty_rect;  /* valid when dirty && !dirty_full */
    uint32_t      frame_seq;   /* increments each completed compose/present */
    int32_t       cursor_x;
    int32_t       cursor_y;
    int           ready;
} gx_server;

int        gx_server_init(void);
gx_server *gx_server_get(void);
void       gx_server_mark_dirty(void);
void       gx_server_mark_dirty_rect(gx_rect r);
/* Live window drag: two rects (old+new), never a fat union (C03/C14). */
void       gx_server_mark_drag_move(gx_rect old_r, gx_rect new_r);
void       gx_server_poll_input(void);
/* Drain PS/2 + move cursor without a full compose/present. Safe on yield. */
void       gx_server_pump_input(void);
/* Compose+scanout if dirty (frame scheduler tick). Safe to call from yield. */
int        gx_server_frame_tick(void);
void       gx_server_present(void);
uint32_t   gx_server_frame_seq(void);

#endif
