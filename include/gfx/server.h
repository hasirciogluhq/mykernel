#ifndef MYKERNEL_GFX_SERVER_H
#define MYKERNEL_GFX_SERVER_H

#include <gfx/device.h>
#include <gfx/compositor.h>
#include <gfx/window.h>
#include <multiboot.h>

typedef struct gx_server {
    gx_device      device;
    gx_compositor comp;
    wm_t          wm;
    gx_surface   *wallpaper;
    gx_surface   *scene;       /* composed frame without cursor */
    gx_surface   *cursor;      /* ARGB cursor glyph */
    int           dirty;
    int32_t       cursor_x;
    int32_t       cursor_y;
    int           ready;
} gx_server;

int        gx_server_init(multiboot_info_t *mbi);
gx_server *gx_server_get(void);
void       gx_server_mark_dirty(void);
void       gx_server_poll_input(void);
void       gx_server_present(void);

#endif
