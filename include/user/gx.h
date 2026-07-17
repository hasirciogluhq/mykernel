#ifndef MYKERNEL_USER_GX_H
#define MYKERNEL_USER_GX_H

#include <kernel/types.h>
#include <kernel/syscall.h>

#define UGX_BTN_LEFT    0x01
#define UGX_BTN_RIGHT   0x02
#define UGX_BTN_MIDDLE  0x04
#define UGX_BTN_BACK    0x08  /* mouse side / X1 */
#define UGX_BTN_FORWARD 0x10  /* mouse side / X2 */

typedef struct ugx_info {
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
} ugx_info;

typedef struct ugx_window_opts {
    int32_t  x, y, w, h;
    int32_t  min_w, min_h, max_w, max_h; /* 0 = unlimited */
    int32_t  radius;
    uint8_t  opacity;                    /* 0..255 */
    char     title[64];
    char     class_name[32];
    int32_t  owner_id;                   /* -1 = none */
    int32_t  parent_id;                  /* -1 = none */

    uint8_t  acrylic;
    uint8_t  rounded;
    uint8_t  alpha;
    uint8_t  background;
    uint8_t  no_drag;
    uint8_t  no_title;
    uint8_t  topmost;
    uint8_t  always_on_bottom;
    uint8_t  resizable;
    uint8_t  fullscreen;
    uint8_t  framed;
    uint8_t  shadow;

    uint8_t  visible;
    uint8_t  minimized;
    uint8_t  maximized;
    uint8_t  closable;
    uint8_t  can_minimize;
    uint8_t  can_maximize;
    uint8_t  accept_focus;
    uint8_t  modal;

    uint8_t  capture_keys;
    uint8_t  capture_mouse;
    uint8_t  mouse_passthrough;
} ugx_window_opts;

typedef struct ugx_frame {
    int32_t x, y, w, h;
} ugx_frame;

typedef struct ugx_map {
    uint32_t *pixels;
    uint32_t  width;
    uint32_t  height;
    uint32_t  stride;
} ugx_map;

typedef struct ugx_fill_args {
    int32_t  win;
    int32_t  x, y, w, h;
    uint32_t color; /* ARGB */
    int32_t  radius; /* for FILL_ROUND */
} ugx_fill_args;

/* Window-local dirty rect for SYS_GX_DAMAGE_RECT */
typedef struct ugx_damage_args {
    int32_t x, y, w, h;
} ugx_damage_args;

typedef struct ugx_wallpaper {
    const uint32_t *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
} ugx_wallpaper;

/* Snapshot after last present/poll (mouse + keyboard focus) */
typedef struct ugx_input_state {
    int32_t mouse_x;
    int32_t mouse_y;
    uint8_t buttons;   /* UGX_BTN_* incl. side buttons when present */
    uint8_t mods;      /* KBD_MOD_* from keyboard.h */
    int32_t focus_id;  /* -1 = none */
    int32_t wheel;     /* accumulated notches since previous input read */
} ugx_input_state;

/* ARGB helpers for userspace */
#define UGX_RGBA(r, g, b, a) \
    ((((uint32_t)(a) & 0xFFu) << 24) | \
     (((uint32_t)(r) & 0xFFu) << 16) | \
     (((uint32_t)(g) & 0xFFu) << 8)  | \
     (((uint32_t)(b) & 0xFFu)))
#define UGX_RGB(r, g, b) UGX_RGBA(r, g, b, 255)

int  ugx_info_get(ugx_info *out);
int  ugx_present(void);
int  ugx_wm_create(const ugx_window_opts *opts);
int  ugx_wm_set(int win, const ugx_window_opts *opts);
int  ugx_wm_get(int win, ugx_window_opts *out);
int  ugx_wm_close(int win);
int  ugx_wm_destroy(int win);
int  ugx_wm_map(int win, ugx_map *out);
int  ugx_wm_focus(int win);
int  ugx_wm_get_frame(int win, ugx_frame *out);
int  ugx_wm_find(const char *title);
int  ugx_fill(int win, int x, int y, int w, int h, uint32_t color);
int  ugx_fill_round(int win, int x, int y, int w, int h, int radius, uint32_t color);
int  ugx_set_wallpaper(const uint32_t *pixels, uint32_t w, uint32_t h, uint32_t stride);
int  ugx_input_get(ugx_input_state *out);
int  ugx_wm_pop_key(int win); /* -1 empty, else Latin-5 byte */
int  ugx_damage(void);        /* mark frame dirty after userspace buffer paint */
int  ugx_damage_rect(int win, int x, int y, int w, int h); /* uses ugx_damage_args */
int  ugx_yield(void);
void ugx_exit(int code);

/* Software helpers on a mapped buffer (userspace CPU draw) */
void ugx_buf_set(ugx_map *m, int x, int y, uint32_t c);
void ugx_buf_fill(ugx_map *m, int x, int y, int w, int h, uint32_t c);
void ugx_buf_rect(ugx_map *m, int x, int y, int w, int h, uint32_t c, int t);
void ugx_buf_text(ugx_map *m, int x, int y, const char *text, uint32_t c);
void ugx_buf_clear(ugx_map *m, uint32_t c);

#endif
