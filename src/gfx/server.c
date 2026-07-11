#include <gfx/server.h>
#include <drivers/fb.h>
#include <drivers/ps2.h>
#include <drivers/keyboard.h>
#include <drivers/mouse.h>
#include <gfx/accel.h>
#include <kernel/string.h>

#define CURSOR_W 12
#define CURSOR_H 19

static gx_server g_server;

/* Classic arrow — 0 empty, 1 outline, 2 fill (gfx pixels, not text) */
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
    if (!s)
        return NULL;
    gx_surface_clear(s, GX_TRANSPARENT);
    for (int y = 0; y < CURSOR_H; y++) {
        for (int x = 0; x < CURSOR_W; x++) {
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
    gx_surface *bb = s->device.backbuffer;
    clamp_cursor_rect(&x, &y, &w, &h, bb->width, bb->height);
    if (w <= 0 || h <= 0)
        return;
    fb_present_rect(bb->pixels, bb->stride, (uint32_t)x, (uint32_t)y,
                    (uint32_t)w, (uint32_t)h);
}

void gx_server_mark_dirty(void)
{
    if (g_server.ready)
        g_server.dirty = 1;
}

void gx_server_poll_input(void)
{
    gx_server *s = gx_server_get();
    if (!s)
        return;

    ps2_poll();

    /* Coalesce moves — only apply the latest position */
    mouse_event_t ev;
    mouse_event_t last_move;
    int have_move = 0;
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
        int before = s->dirty;
        wm_on_mouse_move(&s->wm, last_move.x, last_move.y);
        if (s->wm.drag_id >= 0)
            gx_server_mark_dirty();
        (void)before;
    }

    int ch;
    while ((ch = keyboard_getchar()) >= 0)
        wm_push_key(&s->wm, (uint8_t)ch);
}

int gx_server_init(multiboot_info_t *mbi)
{
    memset(&g_server, 0, sizeof(g_server));

    if (fb_init(mbi) < 0)
        return -1;
    if (gx_device_init(&g_server.device) < 0)
        return -1;
    if (gx_compositor_init(&g_server.comp, &g_server.device) < 0)
        return -1;
    if (wm_init(&g_server.wm, &g_server.comp) < 0)
        return -1;

    g_server.scene = gx_surface_create(g_server.device.fb->width,
                                       g_server.device.fb->height);
    if (!g_server.scene)
        return -1;

    g_server.cursor = make_cursor();
    if (!g_server.cursor)
        return -1;

    ps2_init();
    keyboard_init();
    mouse_set_bounds((int32_t)g_server.device.fb->width,
                     (int32_t)g_server.device.fb->height);
    mouse_init();

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

void gx_server_present(void)
{
    gx_server *s = gx_server_get();
    if (!s)
        return;

    gx_server_poll_input();

    const mouse_state_t *ms = mouse_get();
    int32_t mx = ms ? ms->x : 0;
    int32_t my = ms ? ms->y : 0;
    int mouse_moved = (mx != s->cursor_x || my != s->cursor_y);

    gx_surface *bb = s->device.backbuffer;
    gx_surface *scene = s->scene;

    if (s->dirty) {
        /* Compose into device backbuffer, then snapshot as scene (no cursor). */
        gx_compositor_compose(&s->comp);
        memcpy(scene->pixels, bb->pixels,
               (size_t)scene->stride * scene->height * sizeof(gx_color));

        draw_cursor_on_bb(s, mx, my);
        fb_present(bb->pixels, bb->stride);

        s->cursor_x = mx;
        s->cursor_y = my;
        s->dirty = 0;
        return;
    }

    if (!mouse_moved)
        return;

    /* Mouse-only: restore old cursor footprint from scene, draw at new pos */
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
