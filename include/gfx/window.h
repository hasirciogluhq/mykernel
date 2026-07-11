#ifndef MYKERNEL_GFX_WINDOW_H
#define MYKERNEL_GFX_WINDOW_H

#include <gfx/compositor.h>
#include <gfx/surface.h>

#define WM_MAX_WINDOWS     16
#define WM_TITLE_MAX       64
#define WM_DEFAULT_RADIUS  12
#define WM_TITLEBAR_H      28
#define WM_KEYBUF_SIZE     64

typedef enum {
    WM_STYLE_OPAQUE     = 0,
    WM_STYLE_ACRYLIC    = 1 << 0,
    WM_STYLE_ROUNDED    = 1 << 1,
    WM_STYLE_ALPHA      = 1 << 2,
    WM_STYLE_BACKGROUND = 1 << 3, /* wallpaper — no focus / no drag */
    WM_STYLE_NO_DRAG    = 1 << 4, /* dock etc. */
    WM_STYLE_NO_TITLE   = 1 << 5  /* no titlebar hit zone */
} wm_style_flags;

/* Client-drawn window: userspace owns all pixels (no kernel chrome). */
typedef struct wm_window {
    int         used;
    int         id;
    int         layer_id;
    char        title[WM_TITLE_MAX];
    gx_rect     frame;
    gx_surface *surface;   /* mapped to userspace via SYS_WM_MAP */
    uint32_t    style;
    int32_t     radius;
    int         focused;
    int         visible;
    int         owner_pid;
    /* per-window keyboard input queue (ISO-8859-9 bytes) */
    uint8_t     keybuf[WM_KEYBUF_SIZE];
    unsigned    key_r;
    unsigned    key_w;
} wm_window;

typedef struct wm {
    gx_compositor *comp;
    wm_window      windows[WM_MAX_WINDOWS];
    int            focus_id;       /* keyboard + visual focus */
    int            next_id;
    int            drag_id;        /* -1 = not dragging */
    int32_t        drag_off_x;
    int32_t        drag_off_y;
} wm_t;

typedef struct wm_create_args {
    int32_t  x, y, w, h;
    uint32_t style;
    int32_t  radius;
    char     title[WM_TITLE_MAX];
} wm_create_args;

typedef struct wm_map_info {
    uint32_t *pixels;
    uint32_t  width;
    uint32_t  height;
    uint32_t  stride;
} wm_map_info;

int        wm_init(wm_t *wm, gx_compositor *comp);
void       wm_shutdown(wm_t *wm);
wm_window *wm_create(wm_t *wm, const wm_create_args *args, int owner_pid);
void       wm_destroy(wm_t *wm, int id);
wm_window *wm_get(wm_t *wm, int id);
int        wm_map(wm_t *wm, int id, wm_map_info *out);
void       wm_move(wm_t *wm, int id, int32_t x, int32_t y);
void       wm_resize(wm_t *wm, int id, int32_t w, int32_t h);
void       wm_focus(wm_t *wm, int id);
void       wm_show(wm_t *wm, int id, int visible);
void       wm_sync_layer(wm_t *wm, int id);

/* Input / hit-testing */
int        wm_hit_test(wm_t *wm, int32_t x, int32_t y); /* topmost id or -1 */
int        wm_focused_id(wm_t *wm);
void       wm_on_mouse_move(wm_t *wm, int32_t x, int32_t y);
void       wm_on_mouse_button(wm_t *wm, uint8_t button, int pressed,
                              int32_t x, int32_t y);
int        wm_push_key(wm_t *wm, uint8_t ch); /* to focused; 1=ok */
int        wm_pop_key(wm_t *wm, int id);      /* -1 empty */

#endif
