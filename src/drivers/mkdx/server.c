#include "server.h"
#include "console.h"
#include <drivers/driver.h>
#include <drivers/display.h>
#include <drivers/keyboard.h>
#include <drivers/mouse.h>
#include <drivers/serial.h>
#include "accel.h"
#include "compositor.h"
#include <kernel/string.h>

#define CURSOR_W 12
#define CURSOR_H 19

static gx_server g_server;
/* Set while composing/full-presenting so pump_input won't fight the frame. */
static int g_present_busy;

/* Classic arrow — 0 empty, 1 outline, 2 fill */
static const uint8_t cursor_mask[CURSOR_H][CURSOR_W] = {
    {1,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0},
    {1,2,2,2,2,2,2,2,1,0,0,0},
    {1,2,2,2,2,2,2,2,2,1,0,0},
    {1,2,2,2,2,2,1,1,1,1,1,0},
    {1,2,2,1,2,2,1,0,0,0,0,0},
    {1,2,1,0,1,2,2,1,0,0,0,0},
    {1,1,0,0,1,2,2,1,0,0,0,0},
    {1,0,0,0,0,1,2,2,1,0,0,0},
    {0,0,0,0,0,1,2,2,1,0,0,0},
    {0,0,0,0,0,0,1,2,2,1,0,0},
    {0,0,0,0,0,0,1,2,2,1,0,0},
    {0,0,0,0,0,0,0,1,1,0,0,0},
};

static gx_surface *make_cursor(void)
{
    gx_surface *s = gx_surface_create(CURSOR_W, CURSOR_H);
    int y, x;
    if (!s)
        return NULL;
    gx_surface_clear(s, GX_TRANSPARENT);
    for (y = 0; y < CURSOR_H; y++) {
        for (x = 0; x < CURSOR_W; x++) {
            uint8_t m = cursor_mask[y][x];
            if (m == 1)
                s->pixels[y * s->stride + x] = GX_RGB(10, 10, 10);
            else if (m == 2)
                s->pixels[y * s->stride + x] = GX_RGB(255, 255, 255);
        }
    }
    return s;
}

static void clamp_cursor_rect(int32_t *x, int32_t *y, int32_t *w, int32_t *h,
                              uint32_t sw, uint32_t sh)
{
    if (*x < 0) {
        *w += *x;
        *x = 0;
    }
    if (*y < 0) {
        *h += *y;
        *y = 0;
    }
    if (*x + *w > (int32_t)sw)
        *w = (int32_t)sw - *x;
    if (*y + *h > (int32_t)sh)
        *h = (int32_t)sh - *y;
    if (*w < 0)
        *w = 0;
    if (*h < 0)
        *h = 0;
}

static void blit_scene_rect_to_bb(gx_server *s, int32_t x, int32_t y, int32_t w, int32_t h)
{
    gx_surface *scene = s->scene;
    gx_surface *bb = s->device.backbuffer;
    clamp_cursor_rect(&x, &y, &w, &h, bb->width, bb->height);
    if (w <= 0 || h <= 0)
        return;
    for (int32_t row = 0; row < h; row++) {
        memcpy(&bb->pixels[(uint32_t)(y + row) * bb->stride + (uint32_t)x],
               &scene->pixels[(uint32_t)(y + row) * scene->stride + (uint32_t)x],
               (size_t)w * sizeof(gx_color));
    }
}

static void draw_cursor_on_bb(gx_server *s, int32_t x, int32_t y)
{
    if (!s->cursor)
        return;
    gx_accel_blit(s->device.backbuffer, x, y, s->cursor);
}

static void present_rect(gx_server *s, int32_t x, int32_t y, int32_t w, int32_t h)
{
    display_ops_t *ops = display_active();
    gx_surface *bb = s->device.backbuffer;
    clamp_cursor_rect(&x, &y, &w, &h, bb->width, bb->height);
    if (w <= 0 || h <= 0 || !ops)
        return;
    if (ops->present_rect)
        ops->present_rect(bb->pixels, bb->stride, (uint32_t)x, (uint32_t)y,
                          (uint32_t)w, (uint32_t)h);
    else if (ops->present)
        ops->present(bb->pixels, bb->stride);
}

void gx_server_mark_dirty(void)
{
    if (g_server.ready)
        g_server.dirty = 1;
}

static void flush_cursor_at(gx_server *s, int32_t mx, int32_t my)
{
    display_ops_t *ops;

    if (!s || !s->scene)
        return;
    if (mx == s->cursor_x && my == s->cursor_y)
        return;

    ops = display_active();
    if (!ops)
        return;

    if (s->cursor_x >= 0 && s->cursor_y >= 0) {
        blit_scene_rect_to_bb(s, s->cursor_x, s->cursor_y, CURSOR_W, CURSOR_H);
        present_rect(s, s->cursor_x, s->cursor_y, CURSOR_W, CURSOR_H);
    }

    blit_scene_rect_to_bb(s, mx, my, CURSOR_W, CURSOR_H);
    draw_cursor_on_bb(s, mx, my);
    present_rect(s, mx, my, CURSOR_W, CURSOR_H);

    s->cursor_x = mx;
    s->cursor_y = my;
}

void gx_server_poll_input(void)
{
    gx_server *s = gx_server_get();
    mouse_event_t ev;
    mouse_event_t last_move;
    int have_move = 0;
    int ch;

    if (!s)
        return;

    drivers_poll();

    while (mouse_pop_event(&ev)) {
        if (ev.type == MOUSE_EV_MOVE) {
            last_move = ev;
            have_move = 1;
        } else if (ev.type == MOUSE_EV_DOWN) {
            if (have_move) {
                wm_on_mouse_move(&s->wm, last_move.x, last_move.y);
                have_move = 0;
            }
            wm_on_mouse_button(&s->wm, ev.button, 1, ev.x, ev.y);
            gx_server_mark_dirty();
        } else if (ev.type == MOUSE_EV_UP) {
            if (have_move) {
                wm_on_mouse_move(&s->wm, last_move.x, last_move.y);
                have_move = 0;
            }
            wm_on_mouse_button(&s->wm, ev.button, 0, ev.x, ev.y);
            gx_server_mark_dirty();
        }
    }
    if (have_move) {
        wm_on_mouse_move(&s->wm, last_move.x, last_move.y);
        if (s->wm.drag_id >= 0)
            gx_server_mark_dirty();
    }

    while ((ch = keyboard_getchar()) >= 0)
        wm_push_key(&s->wm, (uint8_t)ch);
}

void gx_server_pump_input(void)
{
    gx_server *s = gx_server_get();
    const mouse_state_t *ms;
    int32_t mx, my;

    if (!s)
        return;

    gx_server_poll_input();

    /* Don't redraw cursor while compose/full-present owns the backbuffer. */
    if (g_present_busy)
        return;

    ms = mouse_get();
    mx = ms ? ms->x : 0;
    my = ms ? ms->y : 0;
    flush_cursor_at(s, mx, my);
}

int gx_server_init(void)
{
    memset(&g_server, 0, sizeof(g_server));

    if (!display_active())
        return -1;

    if (gx_device_init(&g_server.device) < 0)
        return -1;
    if (gx_compositor_init(&g_server.comp, &g_server.device) < 0)
        return -1;
    if (wm_init(&g_server.wm, &g_server.comp) < 0)
        return -1;
    if (proc_console_init() < 0)
        return -1;

    g_server.scene = gx_surface_create(g_server.device.mode.width,
                                       g_server.device.mode.height);
    if (!g_server.scene)
        return -1;

    g_server.cursor = make_cursor();
    if (!g_server.cursor)
        return -1;

    mouse_set_bounds((int32_t)g_server.device.mode.width,
                     (int32_t)g_server.device.mode.height);

    g_server.cursor_x = -1;
    g_server.cursor_y = -1;
    g_server.dirty = 1;
    g_server.ready = 1;
    return 0;
}

gx_server *gx_server_get(void)
{
    return g_server.ready ? &g_server : NULL;
}

static void present_frame_banded(gx_server *s, display_ops_t *ops,
                                 gx_surface *bb, int32_t *mx, int32_t *my)
{
    const uint32_t band_h = 48;
    uint32_t y;
    const mouse_state_t *ms;

    if (!ops->present_rect) {
        g_present_busy = 1;
        ops->present(bb->pixels, bb->stride);
        g_present_busy = 0;
        return;
    }

    /* Present in horizontal bands; between bands drain PS/2 and move cursor
     * so a dirty full-frame blit cannot freeze the pointer for the whole copy. */
    for (y = 0; y < bb->height; y += band_h) {
        uint32_t h = band_h;
        if (y + h > bb->height)
            h = bb->height - y;

        g_present_busy = 1;
        ops->present_rect(bb->pixels, bb->stride, 0, y, bb->width, h);
        g_present_busy = 0;

        gx_server_poll_input();
        ms = mouse_get();
        if (ms && (ms->x != *mx || ms->y != *my)) {
            flush_cursor_at(s, ms->x, ms->y);
            *mx = ms->x;
            *my = ms->y;
        }
    }
}

void gx_server_present(void)
{
    gx_server *s = gx_server_get();
    display_ops_t *ops;
    const mouse_state_t *ms;
    int32_t mx, my;
    gx_surface *bb;
    gx_surface *scene;

    if (!s)
        return;

    /* Live cursor first — even if a dirty compose follows. */
    gx_server_pump_input();

    ops = display_active();
    if (!ops || !ops->present)
        return;

    ms = mouse_get();
    mx = ms ? ms->x : 0;
    my = ms ? ms->y : 0;

    bb = s->device.backbuffer;
    scene = s->scene;

    if (s->dirty) {
        static int s_present_log;
        int li, nvis = 0;

        g_present_busy = 1;
        gx_compositor_compose(&s->comp);
        g_present_busy = 0;

        /* Compose can take a long time — drain PS/2 and take latest pointer. */
        gx_server_poll_input();
        ms = mouse_get();
        mx = ms ? ms->x : 0;
        my = ms ? ms->y : 0;

        if (s_present_log < 3) {
            for (li = 0; li < GX_MAX_LAYERS; li++) {
                gx_layer *L = gx_compositor_layer(&s->comp, li);
                if (!L || !L->used || !L->visible)
                    continue;
                nvis++;
                klog("[gx] layer ");
                serial_print_uint((uint32_t)li);
                klog(" z=");
                serial_print_uint((uint32_t)L->z);
                klog(" ");
                serial_print_uint((uint32_t)L->bounds.w);
                klog("x");
                serial_print_uint((uint32_t)L->bounds.h);
                klog(" @");
                serial_print_uint((uint32_t)L->bounds.x);
                klog(",");
                serial_print_uint((uint32_t)L->bounds.y);
                klog(" px0=");
                if (L->surface && L->surface->pixels)
                    serial_print_hex(L->surface->pixels[0]);
                else
                    klog("null");
                klog("\n");
            }
            klog("[gx] present nvis=");
            serial_print_uint((uint32_t)nvis);
            klog(" bb0=");
            serial_print_hex(bb->pixels[0]);
            klog(" bb_menubar=");
            if (bb->height > 2 && bb->width > 2)
                serial_print_hex(bb->pixels[2 * bb->stride + 2]);
            klog("\n");
            s_present_log++;
        }

        memcpy(scene->pixels, bb->pixels,
               (size_t)scene->stride * scene->height * sizeof(gx_color));

        draw_cursor_on_bb(s, mx, my);
        present_frame_banded(s, ops, bb, &mx, &my);

        s->cursor_x = mx;
        s->cursor_y = my;
        s->dirty = 0;

        /* Apply any motion that arrived on the last band. */
        gx_server_poll_input();
        ms = mouse_get();
        if (ms)
            flush_cursor_at(s, ms->x, ms->y);
        return;
    }

    flush_cursor_at(s, mx, my);
}
