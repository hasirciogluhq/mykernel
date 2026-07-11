#ifndef MYKERNEL_USER_GX_H
#define MYKERNEL_USER_GX_H

#include <kernel/types.h>

/* Graphics / window syscalls (mykernel private range) */
#define SYS_GX_INFO          200
#define SYS_GX_PRESENT       201
#define SYS_WM_CREATE        202
#define SYS_WM_DESTROY       203
#define SYS_WM_MAP           204
#define SYS_WM_MOVE          205
#define SYS_WM_RESIZE        206
#define SYS_WM_FOCUS         207
#define SYS_WM_SHOW          208
#define SYS_GX_FILL          209
#define SYS_GX_FILL_ROUND    210
#define SYS_GX_SET_WALLPAPER 211
#define SYS_INPUT_STATE      212
#define SYS_WM_POP_KEY       213
#define SYS_GX_DAMAGE        214
#define SYS_WM_GET_FRAME     215

#define UGX_STYLE_OPAQUE     0
#define UGX_STYLE_ACRYLIC    (1u << 0)
#define UGX_STYLE_ROUNDED    (1u << 1)
#define UGX_STYLE_ALPHA      (1u << 2)
#define UGX_STYLE_BACKGROUND (1u << 3)
#define UGX_STYLE_NO_DRAG    (1u << 4)
#define UGX_STYLE_NO_TITLE   (1u << 5)

#define UGX_BTN_LEFT   0x01
#define UGX_BTN_RIGHT  0x02
#define UGX_BTN_MIDDLE 0x04

typedef struct ugx_info {
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
} ugx_info;

typedef struct ugx_win_create {
    int32_t  x, y, w, h;
    uint32_t style;
    int32_t  radius;
    char     title[64];
} ugx_win_create;

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
    uint8_t buttons;
    uint8_t mods;      /* KBD_MOD_* from keyboard.h */
    int32_t focus_id;  /* -1 = none */
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
int  ugx_wm_create(const ugx_win_create *args);
int  ugx_wm_destroy(int win);
int  ugx_wm_map(int win, ugx_map *out);
int  ugx_wm_move(int win, int x, int y);
int  ugx_wm_resize(int win, int w, int h);
int  ugx_wm_focus(int win);
int  ugx_wm_show(int win, int visible);
int  ugx_wm_get_frame(int win, ugx_frame *out);
int  ugx_fill(int win, int x, int y, int w, int h, uint32_t color);
int  ugx_fill_round(int win, int x, int y, int w, int h, int radius, uint32_t color);
int  ugx_set_wallpaper(const uint32_t *pixels, uint32_t w, uint32_t h, uint32_t stride);
int  ugx_input_get(ugx_input_state *out);
int  ugx_wm_pop_key(int win); /* -1 empty, else Latin-5 byte */
int  ugx_damage(void);        /* mark frame dirty after userspace buffer paint */

/* Software helpers on a mapped buffer (userspace CPU draw) */
void ugx_buf_set(ugx_map *m, int x, int y, uint32_t c);
void ugx_buf_fill(ugx_map *m, int x, int y, int w, int h, uint32_t c);
void ugx_buf_rect(ugx_map *m, int x, int y, int w, int h, uint32_t c, int t);
void ugx_buf_text(ugx_map *m, int x, int y, const char *text, uint32_t c);
void ugx_buf_clear(ugx_map *m, uint32_t c);

void user_os_ui_main(void);

#endif
