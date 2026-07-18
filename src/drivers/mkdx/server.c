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
#include <kernel/sync.h>

#define CURSOR_W 12
#define CURSOR_H 19

static gx_server g_server;
/*
 * Set for the whole compose→scanout critical section. While set:
 *  - WM drag geometry is deferred (avoids mid-frame layer moves)
 *  - new damage accumulates into pending_* (never cleared with this frame)
 */
static int g_frame_busy;
static int g_tick_depth;
static int g_pending_dirty;
static int g_pending_full;
static gx_rect g_pending_rect;
static int g_deferred_move;
static int32_t g_deferred_mx, g_deferred_my;
/* Button press deferred across compose; release is never deferred (I14 grab). */
static int g_deferred_btn;
static uint8_t g_deferred_btn_code;
static int32_t g_deferred_btn_x, g_deferred_btn_y;
/* Cursor hops during g_frame_busy — apply as soon as the frame releases bb. */
static int g_pending_cursor;
static int32_t g_pending_cx, g_pending_cy;
/* Last pointer we notified — collapse duplicate poll_input MOVE wakes. */
static int g_have_last_notify;
static int32_t g_last_notify_mx, g_last_notify_my;
/* Drag damage as two rects — never the fat old∪new bbox (that was the hitch). */
static int g_drag_damage;
static gx_rect g_drag_old;
static gx_rect g_drag_new;

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

/* Recompose a screen rect into the backbuffer (cursor-free). */
static void compose_rect_to_bb(gx_server *s, int32_t x, int32_t y, int32_t w, int32_t h)
{
    gx_surface *bb = s->device.backbuffer;
    clamp_cursor_rect(&x, &y, &w, &h, bb->width, bb->height);
    if (w <= 0 || h <= 0)
        return;
    gx_compositor_compose_rect(&s->comp, bb, gx_rect_make(x, y, w, h));
}

/* During drag, cursor erase must not wallpaper-punch the scene. */
static gx_color g_cursor_under[CURSOR_W * CURSOR_H];
static int g_cursor_under_valid;
static int32_t g_cursor_under_x, g_cursor_under_y;

static void cursor_under_restore(gx_server *s)
{
    gx_surface *bb;
    int32_t y, x, w, h;

    if (!g_cursor_under_valid || !s || !s->device.backbuffer)
        return;
    bb = s->device.backbuffer;
    x = g_cursor_under_x;
    y = g_cursor_under_y;
    w = CURSOR_W;
    h = CURSOR_H;
    clamp_cursor_rect(&x, &y, &w, &h, bb->width, bb->height);
    for (int32_t row = 0; row < h; row++) {
        memcpy(&bb->pixels[(uint32_t)(y + row) * bb->stride + (uint32_t)x],
               &g_cursor_under[(uint32_t)row * CURSOR_W],
               (size_t)w * sizeof(gx_color));
    }
    g_cursor_under_valid = 0;
}

static void cursor_under_save(gx_server *s, int32_t mx, int32_t my)
{
    gx_surface *bb;
    int32_t x, y, w, h;

    if (!s || !s->device.backbuffer)
        return;
    bb = s->device.backbuffer;
    x = mx;
    y = my;
    w = CURSOR_W;
    h = CURSOR_H;
    clamp_cursor_rect(&x, &y, &w, &h, bb->width, bb->height);
    memset(g_cursor_under, 0, sizeof(g_cursor_under));
    for (int32_t row = 0; row < h; row++) {
        memcpy(&g_cursor_under[(uint32_t)row * CURSOR_W],
               &bb->pixels[(uint32_t)(y + row) * bb->stride + (uint32_t)x],
               (size_t)w * sizeof(gx_color));
    }
    g_cursor_under_x = x;
    g_cursor_under_y = y;
    g_cursor_under_valid = 1;
}

static void draw_cursor_on_bb(gx_server *s, int32_t x, int32_t y)
{
    if (!s->cursor)
        return;
    gx_accel_blit(s->device.backbuffer, x, y, s->cursor);
}

/* Must run before drag_slide / underlay capture — never drop valid without restore. */
static void cursor_erase_from_bb(gx_server *s)
{
    if (g_cursor_under_valid)
        cursor_under_restore(s);
    if (s) {
        s->cursor_x = -1;
        s->cursor_y = -1;
    }
}

static void cursor_paint_at(gx_server *s, int32_t mx, int32_t my)
{
    cursor_under_save(s, mx, my);
    draw_cursor_on_bb(s, mx, my);
    s->cursor_x = mx;
    s->cursor_y = my;
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

#define PRESENT_BATCH_MAX 8

static void present_rect_list(gx_server *s, const gx_rect *rs, int n)
{
    display_ops_t *ops = display_active();
    gx_surface *bb;
    display_rect_t dr[PRESENT_BATCH_MAX];
    int i, m;

    if (!s || !rs || n <= 0 || !ops)
        return;
    bb = s->device.backbuffer;
    if (!bb)
        return;

    m = 0;
    for (i = 0; i < n && m < PRESENT_BATCH_MAX; i++) {
        int32_t x = rs[i].x, y = rs[i].y, w = rs[i].w, h = rs[i].h;
        clamp_cursor_rect(&x, &y, &w, &h, bb->width, bb->height);
        if (w <= 0 || h <= 0)
            continue;
        dr[m].x = (uint32_t)x;
        dr[m].y = (uint32_t)y;
        dr[m].w = (uint32_t)w;
        dr[m].h = (uint32_t)h;
        m++;
    }
    if (m == 0)
        return;

    if (ops->present_rects) {
        ops->present_rects(bb->pixels, bb->stride, dr, (uint32_t)m);
        return;
    }
    for (i = 0; i < m; i++)
        present_rect(s, (int32_t)dr[i].x, (int32_t)dr[i].y,
                     (int32_t)dr[i].w, (int32_t)dr[i].h);
}

/*
 * Drag scanout damage: never present a multi-hop trail AABB.
 * Small moves → one union rect; large jumps → old + new (+ cursor).
 */
static void present_drag_damage(gx_server *s, gx_rect old_r, gx_rect new_r,
                                int32_t mx, int32_t my)
{
    gx_rect cur = gx_rect_make(mx, my, CURSOR_W, CURSOR_H);
    gx_rect uni, inter;
    gx_rect rs[4];
    int n = 0;
    int32_t a_uni, a_parts;

    uni = gx_rect_union(old_r, new_r);
    uni = gx_rect_union(uni, cur);
    if (gx_rect_empty(uni))
        return;

    inter = gx_rect_intersect(old_r, new_r);
    a_uni = gx_rect_area(uni);
    a_parts = gx_rect_area(old_r) + gx_rect_area(new_r) - gx_rect_area(inter)
              + gx_rect_area(cur);

    /* Union is tight enough (typical small pointer hops). */
    if (a_uni <= a_parts + (a_parts >> 2)) {
        present_rect(s, uni.x, uni.y, uni.w, uni.h);
        return;
    }

    if (!gx_rect_empty(old_r))
        rs[n++] = old_r;
    if (!gx_rect_empty(new_r))
        rs[n++] = new_r;
    if (!gx_rect_empty(cur))
        rs[n++] = cur;
    present_rect_list(s, rs, n);
}

static void pending_mark_full(void)
{
    g_pending_dirty = 1;
    g_pending_full = 1;
}

static void pending_mark_rect(gx_rect r)
{
    gx_rect screen;

    if (g_pending_full) {
        g_pending_dirty = 1;
        return;
    }
    screen = gx_rect_make(0, 0, (int32_t)g_server.device.mode.width,
                          (int32_t)g_server.device.mode.height);
    r = gx_rect_intersect(r, screen);
    if (gx_rect_empty(r))
        return;
    if (!g_pending_dirty || gx_rect_empty(g_pending_rect))
        g_pending_rect = r;
    else
        g_pending_rect = gx_rect_union(g_pending_rect, r);
    g_pending_dirty = 1;
}

static void merge_pending_into_dirty(gx_server *s)
{
    if (!g_pending_dirty)
        return;
    if (g_pending_full)
        gx_server_mark_dirty();
    else
        gx_server_mark_dirty_rect(g_pending_rect);
    g_pending_dirty = 0;
    g_pending_full = 0;
    g_pending_rect = gx_rect_make(0, 0, 0, 0);
    (void)s;
}

void gx_server_mark_dirty(void)
{
    if (!g_server.ready)
        return;
    /* App repaints during drag stay pending — don't steal the slide frame. */
    if (g_frame_busy || g_server.wm.drag_id >= 0) {
        pending_mark_full();
        return;
    }
    g_server.dirty = 1;
    g_server.dirty_full = 1;
}

void gx_server_mark_dirty_rect(gx_rect r)
{
    gx_rect screen;
    int32_t screen_area;
    int32_t union_area;

    if (!g_server.ready)
        return;
    /* Sibling/app damage during drag → pending; release flush restores frost. */
    if (g_frame_busy || g_server.wm.drag_id >= 0) {
        pending_mark_rect(r);
        return;
    }
    if (g_server.dirty_full) {
        g_server.dirty = 1;
        return;
    }

    screen = gx_rect_make(0, 0, (int32_t)g_server.device.mode.width,
                          (int32_t)g_server.device.mode.height);
    r = gx_rect_intersect(r, screen);
    if (gx_rect_empty(r))
        return;

    if (!g_server.dirty || gx_rect_empty(g_server.dirty_rect))
        g_server.dirty_rect = r;
    else
        g_server.dirty_rect = gx_rect_union(g_server.dirty_rect, r);

    screen_area = gx_rect_area(screen);
    union_area = gx_rect_area(g_server.dirty_rect);
    if (screen_area > 0 && union_area * 5 >= screen_area * 3)
        g_server.dirty_full = 1;

    g_server.dirty = 1;
}

void gx_server_mark_drag_move(gx_rect old_r, gx_rect new_r)
{
    gx_rect screen;

    if (!g_server.ready)
        return;

    screen = gx_rect_make(0, 0, (int32_t)g_server.device.mode.width,
                          (int32_t)g_server.device.mode.height);
    old_r = gx_rect_intersect(old_r, screen);
    new_r = gx_rect_intersect(new_r, screen);

    /* Keep earliest unrestored old + latest dest — never union into dirty_rect. */
    if (!g_drag_damage)
        g_drag_old = old_r;
    g_drag_new = new_r;
    g_drag_damage = 1;
    g_server.dirty = 1;
}

static void flush_cursor_at(gx_server *s, int32_t mx, int32_t my)
{
    int32_t ox, oy;
    gx_rect uni;
    int32_t uni_area;

    if (!s || !s->device.backbuffer)
        return;
    if (mx == s->cursor_x && my == s->cursor_y)
        return;
    if (!display_active())
        return;

    ox = s->cursor_x;
    oy = s->cursor_y;

    /*
     * Software cursor (C21–C23): update backbuffer fully, then scanout.
     * Never present the erase before the new glyph — that flashes a hole
     * on fast moves (erase→present→draw→present).
     */
    if (g_cursor_under_valid) {
        cursor_under_restore(s);
    } else if (ox >= 0 && oy >= 0 && s->wm.drag_id < 0) {
        /* No save-under (post-compose): rebuild the footprint from the scene. */
        compose_rect_to_bb(s, ox, oy, CURSOR_W, CURSOR_H);
    }

    cursor_paint_at(s, mx, my);

    if (ox < 0 || oy < 0) {
        present_rect(s, mx, my, CURSOR_W, CURSOR_H);
        return;
    }

    uni = gx_rect_union(gx_rect_make(ox, oy, CURSOR_W, CURSOR_H),
                        gx_rect_make(mx, my, CURSOR_W, CURSOR_H));
    uni_area = gx_rect_area(uni);
    /* Short hop: one present of the union (atomic, no ghost/hole). */
    if (uni_area <= (CURSOR_W * CURSOR_H) * 4) {
        present_rect(s, uni.x, uni.y, uni.w, uni.h);
    } else {
        /* Far jump: show new first, then clear old — cursor never blanks. */
        present_rect(s, mx, my, CURSOR_W, CURSOR_H);
        present_rect(s, ox, oy, CURSOR_W, CURSOR_H);
    }
}

static void apply_wm_move(gx_server *s, int32_t x, int32_t y)
{
    if (g_frame_busy) {
        /* Keep latest pointer; apply after the frame so old scene stays coherent. */
        g_deferred_move = 1;
        g_deferred_mx = x;
        g_deferred_my = y;
        return;
    }
    wm_on_mouse_move(&s->wm, x, y);
}

static void flush_deferred_move(gx_server *s)
{
    if (!g_deferred_move)
        return;
    g_deferred_move = 0;
    wm_on_mouse_move(&s->wm, g_deferred_mx, g_deferred_my);
}

static void flush_deferred_btn(gx_server *s)
{
    if (!g_deferred_btn)
        return;
    g_deferred_btn = 0;
    wm_on_mouse_button(&s->wm, g_deferred_btn_code, 1,
                       g_deferred_btn_x, g_deferred_btn_y);
}

static void flush_pending_cursor(gx_server *s)
{
    if (!g_pending_cursor || !s || g_frame_busy)
        return;
    g_pending_cursor = 0;
    flush_cursor_at(s, g_pending_cx, g_pending_cy);
}

static void sync_drag_fast_layer(gx_server *s)
{
    wm_window *w;

    if (!s || s->wm.drag_id < 0) {
        gx_compositor_set_drag_layer(-1);
        return;
    }
    w = wm_get(&s->wm, s->wm.drag_id);
    gx_compositor_set_drag_layer(w ? w->layer_id : -1);
}

static void end_drag_if_released(gx_server *s)
{
    const mouse_state_t *ms;
    wm_window *w;
    int drag_id;
    gx_rect frame;

    /* Grab ends on button-up anywhere — even if the UP event was coalesced away. */
    if (!s || s->wm.drag_id < 0)
        return;
    ms = mouse_get();
    if (!ms || (ms->buttons & MOUSE_BTN_LEFT))
        return;

    drag_id = s->wm.drag_id;
    w = wm_get(&s->wm, drag_id);
    frame = w ? w->frame : gx_rect_make(0, 0, 0, 0);

    s->wm.drag_id = -1;
    cursor_erase_from_bb(s);
    gx_compositor_drag_end();
    g_drag_damage = 0;
    /* Fold stashed app damage; prefer window frame over full-screen dirty. */
    merge_pending_into_dirty(s);
    if (!gx_rect_empty(frame))
        gx_server_mark_dirty_rect(frame);
    else
        gx_server_mark_dirty();
}

void gx_server_poll_input(void)
{
    gx_server *s = gx_server_get();
    mouse_event_t ev;
    mouse_event_t last_move;
    int have_move = 0;
    int ch;
    uint32_t flags = 0;
    int prev_hit;
    int prev_focus;
    int hit;
    int focus;
    const mouse_state_t *ms;

    if (!s)
        return;

    prev_hit = wm_hit_test(&s->wm,
                           s->cursor_x, s->cursor_y);
    prev_focus = wm_focused_id(&s->wm);

    drivers_poll();

    while (mouse_pop_event(&ev)) {
        if (ev.type == MOUSE_EV_MOVE) {
            last_move = ev;
            have_move = 1;
            flags |= INPUT_EV_MOVE;
        } else if (ev.type == MOUSE_EV_DOWN) {
            if (have_move) {
                apply_wm_move(s, last_move.x, last_move.y);
                have_move = 0;
            }
            flags |= INPUT_EV_BUTTON;
            /* Focus/raise start damage — defer across compose; never drop the grab. */
            if (g_frame_busy) {
                g_deferred_btn = 1;
                g_deferred_btn_code = ev.button;
                g_deferred_btn_x = ev.x;
                g_deferred_btn_y = ev.y;
            } else {
                wm_on_mouse_button(&s->wm, ev.button, 1, ev.x, ev.y);
            }
        } else if (ev.type == MOUSE_EV_UP) {
            if (have_move) {
                apply_wm_move(s, last_move.x, last_move.y);
                have_move = 0;
            }
            flags |= INPUT_EV_BUTTON;
            /* Press+release while busy: cancel deferred press; always clear drag. */
            if (g_deferred_btn && g_deferred_btn_code == ev.button)
                g_deferred_btn = 0;
            wm_on_mouse_button(&s->wm, ev.button, 0, ev.x, ev.y);
        } else if (ev.type == MOUSE_EV_WHEEL) {
            flags |= INPUT_EV_WHEEL;
        }
    }
    if (have_move) {
        /* wm_move damages old+new frames; do not escalate to full-screen dirty. */
        apply_wm_move(s, last_move.x, last_move.y);
    }

    end_drag_if_released(s);
    sync_drag_fast_layer(s);

    while ((ch = keyboard_getchar()) >= 0) {
        wm_push_key(&s->wm, (uint8_t)ch);
        flags |= INPUT_EV_KEY;
    }

    ms = mouse_get();
    if (ms && ms->wheel != 0)
        flags |= INPUT_EV_WHEEL;

    hit = ms ? wm_hit_test(&s->wm, ms->x, ms->y) : -1;
    focus = wm_focused_id(&s->wm);
    if (hit != prev_hit || focus != prev_focus)
        flags |= INPUT_EV_FOCUS | INPUT_EV_MOVE;

    /*
     * Safety net: position can move (PS/2 drained elsewhere) without a live
     * queue entry — still notify so wait_events(-1) shell wakes for hover.
     */
    if (ms && (ms->x != s->cursor_x || ms->y != s->cursor_y))
        flags |= INPUT_EV_MOVE;

    /*
     * Coalesce: pump+frame_tick may poll twice with the same tip. Cursor still
     * updates every pump; only skip redundant MOVE-only waiter wakes.
     */
    if (ms && flags && !(flags & ~(INPUT_EV_MOVE | INPUT_EV_FOCUS))) {
        if (g_have_last_notify &&
            g_last_notify_mx == ms->x && g_last_notify_my == ms->y &&
            !(flags & INPUT_EV_FOCUS))
            flags &= ~INPUT_EV_MOVE;
        if (flags & INPUT_EV_MOVE) {
            g_have_last_notify = 1;
            g_last_notify_mx = ms->x;
            g_last_notify_my = ms->y;
        }
    } else if (ms && (flags & (INPUT_EV_BUTTON | INPUT_EV_KEY |
                               INPUT_EV_WHEEL | INPUT_EV_WM))) {
        g_have_last_notify = 1;
        g_last_notify_mx = ms->x;
        g_last_notify_my = ms->y;
    }

    if (flags)
        input_event_notify(flags, hit, focus, prev_hit, -1);
}

void gx_server_pump_input(void)
{
    gx_server *s = gx_server_get();
    const mouse_state_t *ms;
    int32_t mx, my;

    if (!s)
        return;

    gx_server_poll_input();

    ms = mouse_get();
    mx = ms ? ms->x : 0;
    my = ms ? ms->y : 0;

    /*
     * C21/C22: cursor must track the pointer even when apps are PROC_BLOCKED.
     * During compose the backbuffer is owned — remember the tip and apply
     * as soon as the frame finishes (see gx_server_frame_tick).
     */
    if (g_frame_busy) {
        g_pending_cursor = 1;
        g_pending_cx = mx;
        g_pending_cy = my;
        return;
    }

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

    /* Compose directly into the display backbuffer — a second full-screen
     * scene surface (~3.6MiB @ 1280×720) starved WM window allocations on the
     * bump heap (files / activity-monitor create failed; terminal barely fit). */
    g_server.cursor = make_cursor();
    if (!g_server.cursor)
        return -1;

    mouse_set_bounds((int32_t)g_server.device.mode.width,
                     (int32_t)g_server.device.mode.height);

    g_server.cursor_x = -1;
    g_server.cursor_y = -1;
    g_server.dirty = 1;
    g_server.dirty_full = 1;
    g_server.dirty_rect = gx_rect_make(0, 0, 0, 0);
    g_server.frame_seq = 0;
    g_server.ready = 1;
    return 0;
}

gx_server *gx_server_get(void)
{
    return g_server.ready ? &g_server : NULL;
}

static void present_full_frame(gx_server *s, display_ops_t *ops,
                               gx_surface *bb, int32_t *mx, int32_t *my)
{
    const mouse_state_t *ms;

    /*
     * Prefer atomic full present: BGA Y_OFFSET page-flip / virtio resource flush.
     * Drivers drain PS/2 during the copy; WM moves stay deferred via g_frame_busy.
     */
    ops->present(bb->pixels, bb->stride);

    gx_server_poll_input();
    ms = mouse_get();
    if (ms && (ms->x != *mx || ms->y != *my)) {
        flush_cursor_at(s, ms->x, ms->y);
        *mx = ms->x;
        *my = ms->y;
    }
}

/*
 * Live drag: one coalesced slide to the pointer tip, then a tight present.
 * Multi-hop trail unions were presenting a path AABB every frame (~1 FPS).
 */
static int frame_tick_drag(gx_server *s, display_ops_t *ops)
{
    const mouse_state_t *ms;
    gx_surface *bb = s->device.backbuffer;
    gx_rect old_r;
    gx_rect new_r;
    wm_window *dw;
    int layer_id = -1;
    int32_t mx, my;

    if (!bb || !ops)
        return -1;

    sync_drag_fast_layer(s);
    dw = wm_get(&s->wm, s->wm.drag_id);
    if (dw)
        layer_id = dw->layer_id;

    /* Cursor off bb before underlay capture — otherwise ghosts bake into drag. */
    cursor_erase_from_bb(s);

    /*
     * Coalesce to the latest pointer once, then a single slide.
     * Mid-slide poll+hop loops ballooned present damage across the trail.
     */
    g_frame_busy = 0;
    gx_server_poll_input();
    flush_deferred_btn(s);
    flush_deferred_move(s);
    end_drag_if_released(s);
    if (s->wm.drag_id < 0) {
        /* Release during coalesce — fall through to normal dirty compose. */
        if (g_tick_depth <= 3)
            return gx_server_frame_tick();
        return 0;
    }
    if (!g_drag_damage) {
        ms = mouse_get();
        mx = ms ? ms->x : 0;
        my = ms ? ms->y : 0;
        cursor_paint_at(s, mx, my);
        present_rect(s, mx, my, CURSOR_W, CURSOR_H);
        return 0;
    }

    sync_drag_fast_layer(s);
    dw = wm_get(&s->wm, s->wm.drag_id);
    layer_id = dw ? dw->layer_id : -1;
    if (layer_id < 0) {
        g_drag_damage = 0;
        return 0;
    }

    old_r = g_drag_old;
    new_r = g_drag_new;
    g_drag_damage = 0;

    g_frame_busy = 1;
    gx_compositor_drag_slide(&s->comp, bb, old_r, new_r, layer_id);

    ms = mouse_get();
    mx = ms ? ms->x : 0;
    my = ms ? ms->y : 0;
    cursor_paint_at(s, mx, my);

    present_drag_damage(s, old_r, new_r, mx, my);

    s->frame_seq++;
    g_frame_busy = 0;

    /* Next moves wait for the following frame_tick — do not nest another slide. */
    gx_server_poll_input();
    flush_deferred_btn(s);
    flush_deferred_move(s);
    end_drag_if_released(s);
    flush_pending_cursor(s);
    ms = mouse_get();
    if (ms && (ms->x != mx || ms->y != my) && s->wm.drag_id < 0)
        flush_cursor_at(s, ms->x, ms->y);

    return 0;
}

/*
 * Independent frame clock (C10/T01): compose+scanout when dirty.
 * Called from present syscall and from yield (via pump) so apps that skip
 * present still get WM move/focus damage painted.
 */
int gx_server_frame_tick(void)
{
    gx_server *s = gx_server_get();
    display_ops_t *ops;
    const mouse_state_t *ms;
    int32_t mx, my;
    gx_surface *bb;
    gx_rect damage;
    int full;
    int r;

    if (!s)
        return -1;

    /* Nested tick / re-enter: only one frame producer. */
    if (g_frame_busy || g_tick_depth > 3)
        return 0;

    g_tick_depth++;

    ops = display_active();
    if (!ops || !ops->present) {
        g_tick_depth--;
        return -1;
    }

    /* Live drag owns the frame clock — ignore sibling dirty until release. */
    if (s->wm.drag_id >= 0) {
        if (g_drag_damage) {
            r = frame_tick_drag(s, ops);
            g_tick_depth--;
            return r;
        }
        gx_server_poll_input();
        ms = mouse_get();
        mx = ms ? ms->x : 0;
        my = ms ? ms->y : 0;
        flush_cursor_at(s, mx, my);
        flush_deferred_btn(s);
        flush_deferred_move(s);
        if (g_drag_damage && g_tick_depth <= 3) {
            r = gx_server_frame_tick();
            g_tick_depth--;
            return r;
        }
        g_tick_depth--;
        return 0;
    }

    /* Drag ended with leftover split damage — fold into normal dirty. */
    if (g_drag_damage) {
        gx_server_mark_dirty_rect(g_drag_old);
        gx_server_mark_dirty_rect(g_drag_new);
        g_drag_damage = 0;
    }

    if (!s->dirty) {
        gx_server_poll_input();
        ms = mouse_get();
        mx = ms ? ms->x : 0;
        my = ms ? ms->y : 0;
        flush_cursor_at(s, mx, my);
        flush_deferred_btn(s);
        flush_deferred_move(s);
        merge_pending_into_dirty(s);
        if (s->dirty && g_tick_depth <= 3) {
            r = gx_server_frame_tick();
            g_tick_depth--;
            return r;
        }
        g_tick_depth--;
        return 0;
    }

    ms = mouse_get();
    mx = ms ? ms->x : 0;
    my = ms ? ms->y : 0;

    bb = s->device.backbuffer;
    if (!bb) {
        g_tick_depth--;
        return -1;
    }

    full = s->dirty_full || gx_rect_empty(s->dirty_rect);
    if (full) {
        damage = gx_rect_make(0, 0, (int32_t)bb->width, (int32_t)bb->height);
    } else {
        damage = gx_rect_intersect(
            s->dirty_rect,
            gx_rect_make(0, 0, (int32_t)bb->width, (int32_t)bb->height));
        if (gx_rect_empty(damage)) {
            s->dirty = 0;
            s->dirty_full = 0;
            flush_cursor_at(s, mx, my);
            g_tick_depth--;
            return 0;
        }
    }

    /* Snapshot damage, then clear live dirty so mid-frame marks go to pending. */
    s->dirty = 0;
    s->dirty_full = 0;
    s->dirty_rect = gx_rect_make(0, 0, 0, 0);

    g_frame_busy = 1;

    {
        static int s_present_log;
        int li, nvis = 0;

        /* Compose straight into the scanout buffer (no spare scene surface). */
        gx_compositor_compose_rect(&s->comp, bb, damage);

        /* Drain PS/2; WM moves stay deferred until frame completes. */
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
            klog(full ? " full" : " partial");
            klog("\n");
            s_present_log++;
        }

        /*
         * Compose left a cursor-free scene inside damage; any save-under is
         * stale. Rebuild the old footprint if it still sits outside damage,
         * then paint + save-under so the next mouse hop skips recompose.
         */
        {
            int32_t ox = s->cursor_x;
            int32_t oy = s->cursor_y;

            g_cursor_under_valid = 0;
            if (ox >= 0 && oy >= 0)
                compose_rect_to_bb(s, ox, oy, CURSOR_W, CURSOR_H);
            cursor_paint_at(s, mx, my);

            if (full) {
                present_full_frame(s, ops, bb, &mx, &my);
            } else {
                gx_rect pr = damage;
                if (ox >= 0 && oy >= 0)
                    pr = gx_rect_union(pr, gx_rect_make(ox, oy, CURSOR_W, CURSOR_H));
                pr = gx_rect_union(pr, gx_rect_make(mx, my, CURSOR_W, CURSOR_H));
                present_rect(s, pr.x, pr.y, pr.w, pr.h);
            }
        }
        s->frame_seq++;
    }

    g_frame_busy = 0;

    /* Apply deferred press/drag + any damage marked during the frame. */
    flush_deferred_btn(s);
    flush_deferred_move(s);
    flush_pending_cursor(s);
    end_drag_if_released(s);
    merge_pending_into_dirty(s);

    /* Catch pointer hops that arrived mid-compose so drag stays glued. */
    if (s->wm.drag_id >= 0 && g_drag_damage && g_tick_depth <= 3) {
        r = gx_server_frame_tick();
        g_tick_depth--;
        return r;
    }

    gx_server_poll_input();
    ms = mouse_get();
    if (ms)
        flush_cursor_at(s, ms->x, ms->y);

    g_tick_depth--;
    return 0;
}

void gx_server_present(void)
{
    gx_server *s = gx_server_get();
    if (!s)
        return;

    /* Pre-frame input (not busy yet — drag applies immediately). */
    gx_server_poll_input();
    gx_server_frame_tick();
}

uint32_t gx_server_frame_seq(void)
{
    return g_server.ready ? g_server.frame_seq : 0;
}
