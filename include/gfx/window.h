#ifndef MYKERNEL_GFX_WINDOW_H
#define MYKERNEL_GFX_WINDOW_H

#include <gfx/compositor.h>
#include <gfx/surface.h>

#define WM_MAX_WINDOWS     16
#define WM_TITLE_MAX       64
#define WM_TITLEBAR_H      28
#define WM_BORDER          1

typedef enum {
    WM_STYLE_NORMAL   = 0,
    WM_STYLE_ACRYLIC  = 1 << 0,  /* frosted glass body */
    WM_STYLE_NO_TITLE = 1 << 1,
    WM_STYLE_NO_BORDER = 1 << 2
} wm_style_flags;

typedef struct wm_window {
    int         used;
    int         id;
    int         layer_id;
    char        title[WM_TITLE_MAX];
    gx_rect     frame;      /* including titlebar */
    gx_rect     client;     /* client area in screen coords */
    gx_surface *client_surf;
    uint32_t    style;
    int         focused;
    int         visible;
    gx_color    accent;
} wm_window;

typedef struct wm {
    gx_compositor *comp;
    wm_window      windows[WM_MAX_WINDOWS];
    int            focus_id; /* window id, -1 = none */
    int            next_id;
} wm_t;

int        wm_init(wm_t *wm, gx_compositor *comp);
void       wm_shutdown(wm_t *wm);
wm_window *wm_create(wm_t *wm, const char *title, gx_rect frame, uint32_t style);
void       wm_destroy(wm_t *wm, int id);
wm_window *wm_get(wm_t *wm, int id);
void       wm_set_title(wm_t *wm, int id, const char *title);
void       wm_move(wm_t *wm, int id, int32_t x, int32_t y);
void       wm_resize(wm_t *wm, int id, int32_t w, int32_t h);
void       wm_focus(wm_t *wm, int id);
void       wm_show(wm_t *wm, int id, int visible);
void       wm_paint_chrome(wm_t *wm, int id); /* redraw titlebar into layer */
void       wm_paint_all(wm_t *wm);
gx_surface *wm_client_surface(wm_t *wm, int id);

#endif
