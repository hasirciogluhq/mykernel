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
    if (g_frame_busy) {
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
    int dragging = g_server.wm.drag_id >= 0;

    if (!g_server.ready)
        return;
    if (g_frame_busy) {
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

    /*
     * Escalate huge unions to a full refresh — but never while dragging.
     * Live window moves must stay old+new rect only (C03/C14) or they hitch.
     */
    if (!dragging) {
        screen_area = gx_rect_area(screen);
        union_area = gx_rect_area(g_server.dirty_rect);
        if (screen_area > 0 && union_area * 5 >= screen_area * 3)
            g_server.dirty_full = 1;
    }

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
    display_ops_t *ops;

    if (!s || !s->device.backbuffer)
        return;
    if (mx == s->cursor_x && my == s->cursor_y)
        return;

    ops = display_active();
    if (!ops)
        return;

    /* Erase old cursor by recomposing under it, then paint at the new spot. */
    if (s->cursor_x >= 0 && s->cursor_y >= 0) {
        compose_rect_to_bb(s, s->cursor_x, s->cursor_y, CURSOR_W, CURSOR_H);
        present_rect(s, s->cursor_x, s->cursor_y, CURSOR_W, CURSOR_H);
    }

    compose_rect_to_bb(s, mx, my, CURSOR_W, CURSOR_H);
    draw_cursor_on_bb(s, mx, my);
    present_rect(s, mx, my, CURSOR_W, CURSOR_H);

    s->cursor_x = mx;
    s->cursor_y = my;
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

static void end_drag_if_released(gx_server *s)
{
    const mouse_state_t *ms;

    /* Grab ends on button-up anywhere — even if the UP event was coalesced away. */
    if (!s || s->wm.drag_id < 0)
        return;
    ms = mouse_get();
    if (ms && !(ms->buttons & MOUSE_BTN_LEFT))
        s->wm.drag_id = -1;
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
                apply_wm_move(s, last_move.x, last_move.y);
                have_move = 0;
            }
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
            /* Press+release while busy: cancel deferred press; always clear drag. */
            if (g_deferred_btn && g_deferred_btn_code == ev.button)
                g_deferred_btn = 0;
            wm_on_mouse_button(&s->wm, ev.button, 0, ev.x, ev.y);
        }
    }
    if (have_move) {
        /* wm_move damages old+new frames; do not escalate to full-screen dirty. */
        apply_wm_move(s, last_move.x, last_move.y);
    }

    end_drag_if_released(s);

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
    if (g_frame_busy)
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
 * Live drag: compose old + new as two window-sized rects (not their bbox).
 * Cursor path stays identical to the normal tick — pump_input is untouched.
 */
static int frame_tick_drag(gx_server *s, display_ops_t *ops)
{
    const mouse_state_t *ms;
    gx_surface *bb = s->device.backbuffer;
    gx_rect old_r = g_drag_old;
    gx_rect new_r = g_drag_new;
    int32_t mx, my;

    if (!bb || !ops)
        return -1;

    g_drag_damage = 0;
    if (!s->dirty_full && gx_rect_empty(s->dirty_rect))
        s->dirty = 0;

    ms = mouse_get();
    mx = ms ? ms->x : 0;
    my = ms ? ms->y : 0;

    g_frame_busy = 1;

    if (!gx_rect_empty(old_r))
        gx_compositor_compose_rect(&s->comp, bb, old_r);
    if (!gx_rect_empty(new_r))
        gx_compositor_compose_rect(&s->comp, bb, new_r);

    gx_server_poll_input();
    ms = mouse_get();
    mx = ms ? ms->x : 0;
    my = ms ? ms->y : 0;

    if (s->cursor_x >= 0 && s->cursor_y >= 0 &&
        (s->cursor_x != mx || s->cursor_y != my))
        compose_rect_to_bb(s, s->cursor_x, s->cursor_y, CURSOR_W, CURSOR_H);
    draw_cursor_on_bb(s, mx, my);

    if (!gx_rect_empty(old_r))
        present_rect(s, old_r.x, old_r.y, old_r.w, old_r.h);
    if (!gx_rect_empty(new_r) &&
        !(new_r.x == old_r.x && new_r.y == old_r.y &&
          new_r.w == old_r.w && new_r.h == old_r.h))
        present_rect(s, new_r.x, new_r.y, new_r.w, new_r.h);

    /* Cursor present separately so we never skip flush_cursor / pump path. */
    present_rect(s, mx, my, CURSOR_W, CURSOR_H);

    s->cursor_x = mx;
    s->cursor_y = my;
    s->frame_seq++;

    g_frame_busy = 0;

    flush_deferred_btn(s);
    flush_deferred_move(s);
    end_drag_if_released(s);
    merge_pending_into_dirty(s);

    if (s->wm.drag_id >= 0 && (s->dirty || g_drag_damage) && g_tick_depth <= 3) {
        int r = gx_server_frame_tick();
        return r;
    }

    gx_server_poll_input();
    ms = mouse_get();
    if (ms)
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

    /* Split drag path — mouse/cursor polling stays on the normal path. */
    if (g_drag_damage && s->wm.drag_id >= 0) {
        r = frame_tick_drag(s, ops);
        g_tick_depth--;
        return r;
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
        if (s->wm.drag_id >= 0 && (s->dirty || g_drag_damage) && g_tick_depth <= 3) {
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

        /* Damage already in bb; restore under old cursor then overlay. */
        if (s->cursor_x >= 0 && s->cursor_y >= 0 &&
            (s->cursor_x != mx || s->cursor_y != my))
            compose_rect_to_bb(s, s->cursor_x, s->cursor_y, CURSOR_W, CURSOR_H);
        draw_cursor_on_bb(s, mx, my);

        if (full) {
            present_full_frame(s, ops, bb, &mx, &my);
        } else {
            gx_rect pr = damage;
            if (s->cursor_x >= 0 && s->cursor_y >= 0)
                pr = gx_rect_union(pr, gx_rect_make(s->cursor_x, s->cursor_y,
                                                    CURSOR_W, CURSOR_H));
            pr = gx_rect_union(pr, gx_rect_make(mx, my, CURSOR_W, CURSOR_H));
            present_rect(s, pr.x, pr.y, pr.w, pr.h);
        }

        s->cursor_x = mx;
        s->cursor_y = my;
        s->frame_seq++;
    }

    g_frame_busy = 0;

    /* Apply deferred press/drag + any damage marked during the frame. */
    flush_deferred_btn(s);
    flush_deferred_move(s);
    end_drag_if_released(s);
    merge_pending_into_dirty(s);

    /* Catch pointer hops that arrived mid-compose so drag stays glued. */
    if (s->wm.drag_id >= 0 && (s->dirty || g_drag_damage) && g_tick_depth <= 3) {
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
