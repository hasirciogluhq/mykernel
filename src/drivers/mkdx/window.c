#include "window.h"
#include "accel.h"
#include "server.h"
#include <kernel/string.h>

static wm_window *slot_by_id(wm_t *wm, int id)
{
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (wm->windows[i].used && wm->windows[i].id == id)
            return &wm->windows[i];
    }
    return NULL;
}

static int effective_visible(const ugx_window_opts *opts)
{
    return opts && opts->visible && !opts->minimized;
}

static int class_name_is(const ugx_window_opts *opts, const char *name)
{
    return opts && name && opts->class_name[0] &&
           strcmp(opts->class_name, name) == 0;
}

static void sanitize_opts(ugx_window_opts *opts)
{
    if (!opts)
        return;

    if (opts->w < 16)
        opts->w = 16;
    if (opts->h < 16)
        opts->h = 16;
    if (opts->min_w < 0)
        opts->min_w = 0;
    if (opts->min_h < 0)
        opts->min_h = 0;
    if (opts->max_w < 0)
        opts->max_w = 0;
    if (opts->max_h < 0)
        opts->max_h = 0;
    if (opts->min_w > 0 && opts->w < opts->min_w)
        opts->w = opts->min_w;
    if (opts->min_h > 0 && opts->h < opts->min_h)
        opts->h = opts->min_h;
    if (opts->max_w > 0 && opts->w > opts->max_w)
        opts->w = opts->max_w;
    if (opts->max_h > 0 && opts->h > opts->max_h)
        opts->h = opts->max_h;
    if (opts->rounded) {
        if (opts->radius <= 0)
            opts->radius = WM_DEFAULT_RADIUS;
    } else {
        opts->radius = 0;
    }
    if (!opts->framed)
        opts->no_title = 1;
    if (opts->background)
        opts->accept_focus = 0;
    if (opts->maximized)
        opts->minimized = 0;
    opts->title[WM_TITLE_MAX - 1] = 0;
    opts->class_name[sizeof(opts->class_name) - 1] = 0;
    if (!opts->title[0])
        strncpy(opts->title, "Window", WM_TITLE_MAX - 1);
}

static gx_rect screen_frame(wm_t *wm)
{
    if (!wm || !wm->comp || !wm->comp->device)
        return gx_rect_make(0, 0, 0, 0);
    return gx_rect_make(0, 0, (int32_t)wm->comp->device->mode.width,
                        (int32_t)wm->comp->device->mode.height);
}

static int resize_surface(wm_window *w, int32_t width, int32_t height)
{
    if (!w || !w->surface)
        return -1;
    if (width == (int32_t)w->surface->width && height == (int32_t)w->surface->height)
        return 0;

    gx_surface *ns = gx_surface_create((uint32_t)width, (uint32_t)height);
    if (!ns)
        return -1;
    gx_accel_blit(ns, 0, 0, w->surface);
    gx_surface_destroy(w->surface);
    w->surface = ns;
    return 0;
}

static void raise_topmost_windows(wm_t *wm)
{
    if (!wm || !wm->comp)
        return;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        wm_window *w = &wm->windows[i];
        if (!w->used || !w->opts.topmost || !effective_visible(&w->opts))
            continue;
        gx_compositor_raise(wm->comp, w->layer_id);
    }
}

static void apply_layer_style(wm_window *w, gx_layer *layer)
{
    layer->corner_radius = w->opts.rounded ? w->opts.radius : 0;
    layer->opacity = w->opts.opacity ? w->opts.opacity : 255;
    layer->blur_radius = 10;

    if (w->opts.acrylic) {
        layer->style = GX_LAYER_ACRYLIC;
        if (class_name_is(&w->opts, "shell.menubar")) {
            layer->tint = GX_RGBA(248, 249, 252, 178);
        } else if (class_name_is(&w->opts, "shell.dock")) {
            layer->tint = GX_RGBA(26, 30, 38, 176);
        } else {
            layer->tint = GX_RGBA(255, 255, 255, 120);
        }
    } else if (w->opts.alpha) {
        layer->style = GX_LAYER_ALPHA;
    } else {
        layer->style = GX_LAYER_OPAQUE;
        layer->opacity = 255;
    }
}

void wm_sync_layer(wm_t *wm, int id)
{
    wm_window *w = slot_by_id(wm, id);
    gx_rect prev;
    if (!w)
        return;
    gx_layer *L = gx_compositor_layer(wm->comp, w->layer_id);
    if (!L)
        return;
    prev = L->bounds;
    L->bounds = w->frame;
    L->surface = w->surface;
    L->visible = effective_visible(&w->opts);
    apply_layer_style(w, L);
    /* Geometry/style sync: damage previous and new bounds (not full screen). */
    gx_server_mark_dirty_rect(prev);
    gx_server_mark_dirty_rect(w->frame);
}

int wm_init(wm_t *wm, gx_compositor *comp)
{
    if (!wm || !comp)
        return -1;
    memset(wm, 0, sizeof(*wm));
    wm->comp = comp;
    wm->focus_id = -1;
    wm->drag_id = -1;
    wm->next_id = 1;
    return 0;
}

void wm_shutdown(wm_t *wm)
{
    if (!wm)
        return;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (wm->windows[i].used)
            wm_destroy(wm, wm->windows[i].id);
    }
}

wm_window *wm_create(wm_t *wm, const ugx_window_opts *opts, int owner_pid)
{
    if (!wm || !opts)
        return NULL;

    wm_window *w = NULL;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (!wm->windows[i].used) {
            w = &wm->windows[i];
            break;
        }
    }
    if (!w)
        return NULL;

    memset(w, 0, sizeof(*w));
    w->used = 1;
    w->id = wm->next_id++;
    w->owner_pid = owner_pid;
    w->opts = *opts;
    sanitize_opts(&w->opts);
    w->frame = gx_rect_make(w->opts.x, w->opts.y, w->opts.w, w->opts.h);

    w->surface = gx_surface_create((uint32_t)w->frame.w, (uint32_t)w->frame.h);
    if (!w->surface) {
        w->used = 0;
        return NULL;
    }
    gx_surface_clear(w->surface, GX_TRANSPARENT);

    gx_layer layer;
    memset(&layer, 0, sizeof(layer));
    layer.visible = 1;
    layer.z = w->opts.always_on_bottom ? 0 : w->id;
    layer.bounds = w->frame;
    layer.surface = w->surface;
    apply_layer_style(w, &layer);

    w->layer_id = gx_compositor_add_layer(wm->comp, &layer);
    if (w->layer_id < 0) {
        gx_surface_destroy(w->surface);
        w->used = 0;
        return NULL;
    }

    if (wm_apply_opts(wm, w->id, &w->opts) < 0) {
        wm_destroy(wm, w->id);
        return NULL;
    }
    if (w->opts.topmost)
        raise_topmost_windows(wm);
    if (effective_visible(&w->opts) && w->opts.accept_focus && !w->opts.background)
        wm_focus(wm, w->id);
    gx_server_mark_dirty();
    return w;
}

void wm_destroy(wm_t *wm, int id)
{
    wm_window *w = slot_by_id(wm, id);
    if (!w)
        return;

    gx_compositor_remove_layer(wm->comp, w->layer_id);
    if (w->surface)
        gx_surface_destroy(w->surface);
    if (wm->focus_id == id)
        wm->focus_id = -1;
    if (wm->drag_id == id)
        wm->drag_id = -1;
    memset(w, 0, sizeof(*w));
    gx_server_mark_dirty();
}

int wm_close(wm_t *wm, int id)
{
    if (!slot_by_id(wm, id))
        return -1;
    wm_destroy(wm, id);
    return 0;
}

void wm_destroy_by_pid(wm_t *wm, int pid)
{
    if (!wm || pid <= 0)
        return;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        wm_window *w = &wm->windows[i];
        if (w->used && w->owner_pid == pid)
            wm_destroy(wm, w->id);
    }
}

wm_window *wm_get(wm_t *wm, int id)
{
    return slot_by_id(wm, id);
}

int wm_apply_opts(wm_t *wm, int id, const ugx_window_opts *opts)
{
    wm_window *w = slot_by_id(wm, id);
    if (!wm || !w || !opts)
        return -1;

    ugx_window_opts next = *opts;
    sanitize_opts(&next);

    gx_rect frame = w->frame;
    const int was_maximized = w->opts.maximized || w->opts.fullscreen;
    const int want_maximized = next.maximized || next.fullscreen;

    if (want_maximized && !was_maximized) {
        w->restore_frame = w->frame;
        w->has_restore_frame = 1;
    }

    if (want_maximized) {
        frame = screen_frame(wm);
    } else if (was_maximized && w->has_restore_frame) {
        frame = w->restore_frame;
    } else {
        frame.x = next.x;
        frame.y = next.y;
        frame.w = next.w;
        frame.h = next.h;
    }

    if (frame.w < 16)
        frame.w = 16;
    if (frame.h < 16)
        frame.h = 16;
    if (next.min_w > 0 && frame.w < next.min_w)
        frame.w = next.min_w;
    if (next.min_h > 0 && frame.h < next.min_h)
        frame.h = next.min_h;
    if (next.max_w > 0 && frame.w > next.max_w)
        frame.w = next.max_w;
    if (next.max_h > 0 && frame.h > next.max_h)
        frame.h = next.max_h;

    if (resize_surface(w, frame.w, frame.h) < 0)
        return -1;

    w->frame = frame;
    next.x = frame.x;
    next.y = frame.y;
    next.w = frame.w;
    next.h = frame.h;
    w->opts = next;

    if (!w->opts.accept_focus || w->opts.background || !effective_visible(&w->opts)) {
        w->focused = 0;
        if (wm->focus_id == id)
            wm->focus_id = -1;
    }

    wm_sync_layer(wm, id);
    if (w->opts.topmost)
        raise_topmost_windows(wm);
    return 0;
}

int wm_get_opts(wm_t *wm, int id, ugx_window_opts *out)
{
    wm_window *w = slot_by_id(wm, id);
    if (!w || !out)
        return -1;
    *out = w->opts;
    out->x = w->frame.x;
    out->y = w->frame.y;
    out->w = w->frame.w;
    out->h = w->frame.h;
    return 0;
}

int wm_find_by_title(wm_t *wm, const char *title)
{
    int i;
    if (!wm || !title)
        return -1;
    for (i = 0; i < WM_MAX_WINDOWS; i++) {
        wm_window *w = &wm->windows[i];
        if (!w->used)
            continue;
        if (strcmp(w->opts.title, title) == 0)
            return w->id;
    }
    return -1;
}

int wm_map(wm_t *wm, int id, wm_map_info *out)
{
    wm_window *w = slot_by_id(wm, id);
    if (!w || !w->surface || !out)
        return -1;
    out->pixels = w->surface->pixels;
    out->width = w->surface->width;
    out->height = w->surface->height;
    out->stride = w->surface->stride;
    return 0;
}

void wm_move(wm_t *wm, int id, int32_t x, int32_t y)
{
    wm_window *w = slot_by_id(wm, id);
    gx_layer *L;
    gx_rect old;

    if (!w)
        return;

    /* Maximized/fullscreen exit still goes through the full opts path. */
    if (w->opts.maximized || w->opts.fullscreen) {
        ugx_window_opts opts = w->opts;
        opts.maximized = 0;
        opts.fullscreen = 0;
        opts.x = x;
        opts.y = y;
        (void)wm_apply_opts(wm, id, &opts);
        return;
    }

    if (w->frame.x == x && w->frame.y == y)
        return;

    /* Move = transform only: reuse surface, damage old+new frames (C03/T10). */
    old = w->frame;
    w->frame.x = x;
    w->frame.y = y;
    w->opts.x = x;
    w->opts.y = y;
    L = gx_compositor_layer(wm->comp, w->layer_id);
    if (L)
        L->bounds = w->frame;
    gx_server_mark_dirty_rect(old);
    gx_server_mark_dirty_rect(w->frame);
}

void wm_resize(wm_t *wm, int id, int32_t wdt, int32_t hgt)
{
    ugx_window_opts opts;
    wm_window *w = slot_by_id(wm, id);
    if (!w)
        return;
    opts = w->opts;
    opts.maximized = 0;
    opts.w = wdt;
    opts.h = hgt;
    (void)wm_apply_opts(wm, id, &opts);
}

void wm_focus(wm_t *wm, int id)
{
    if (!wm)
        return;

    wm_window *w = slot_by_id(wm, id);
    if (w && (!w->opts.accept_focus || w->opts.background || !effective_visible(&w->opts)))
        return;

    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (wm->windows[i].used)
            wm->windows[i].focused = (wm->windows[i].id == id);
    }
    wm->focus_id = id;
    if (w && !w->opts.always_on_bottom)
        gx_compositor_raise(wm->comp, w->layer_id);
    raise_topmost_windows(wm);
    if (w)
        gx_server_mark_dirty_rect(w->frame);
}

void wm_show(wm_t *wm, int id, int visible)
{
    wm_window *w = slot_by_id(wm, id);
    if (!w)
        return;
    ugx_window_opts opts = w->opts;
    opts.visible = visible ? 1 : 0;
    if (visible)
        opts.minimized = 0;
    (void)wm_apply_opts(wm, id, &opts);
}

int wm_focused_id(wm_t *wm)
{
    return wm ? wm->focus_id : -1;
}

int wm_hit_test(wm_t *wm, int32_t x, int32_t y)
{
    if (!wm || !wm->comp)
        return -1;

    int best_id = -1;
    int best_z = -1;

    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        wm_window *w = &wm->windows[i];
        if (!w->used || !effective_visible(&w->opts))
            continue;
        if (x < w->frame.x || y < w->frame.y ||
            x >= w->frame.x + w->frame.w || y >= w->frame.y + w->frame.h)
            continue;

        gx_layer *L = gx_compositor_layer(wm->comp, w->layer_id);
        int z = L ? L->z : w->id;
        if (z >= best_z) {
            best_z = z;
            best_id = w->id;
        }
    }
    return best_id;
}

void wm_on_mouse_move(wm_t *wm, int32_t x, int32_t y)
{
    if (!wm || wm->drag_id < 0)
        return;
    wm_window *w = slot_by_id(wm, wm->drag_id);
    if (!w)
        return;
    wm_move(wm, wm->drag_id, x - wm->drag_off_x, y - wm->drag_off_y);
}

void wm_on_mouse_button(wm_t *wm, uint8_t button, int pressed,
                        int32_t x, int32_t y)
{
    if (!wm)
        return;

    if (button != 0x01) /* left only for WM chrome */
        return;

    if (!pressed) {
        wm->drag_id = -1;
        return;
    }

    int id = wm_hit_test(wm, x, y);
    if (id < 0)
        return;

    wm_window *w = slot_by_id(wm, id);
    if (!w)
        return;

    if (w->opts.background || !w->opts.accept_focus)
        return;

    wm_focus(wm, id);

    int can_drag = !(w->opts.no_drag || w->opts.no_title);
    int local_x = x - w->frame.x;
    int local_y = y - w->frame.y;
    int in_title = local_y >= 0 && local_y < WM_TITLEBAR_H;
    int in_chrome_btns = in_title && local_x >= 0 && local_x < WM_CHROME_BTN_ZONE &&
                         (w->opts.closable || w->opts.can_minimize || w->opts.can_maximize);
    if (can_drag && in_title && !in_chrome_btns) {
        wm->drag_id = id;
        wm->drag_off_x = local_x;
        wm->drag_off_y = local_y;
    }
}

int wm_push_key(wm_t *wm, uint8_t ch)
{
    if (!wm || wm->focus_id < 0)
        return 0;
    wm_window *w = slot_by_id(wm, wm->focus_id);
    if (!w || w->opts.background ||
        (!w->opts.capture_keys && !w->opts.accept_focus))
        return 0;

    unsigned next = (w->key_w + 1) % WM_KEYBUF_SIZE;
    if (next == w->key_r)
        return 0;
    w->keybuf[w->key_w] = ch;
    w->key_w = next;
    return 1;
}

int wm_pop_key(wm_t *wm, int id)
{
    wm_window *w = slot_by_id(wm, id);
    if (!w || w->key_r == w->key_w)
        return -1;
    uint8_t c = w->keybuf[w->key_r];
    w->key_r = (w->key_r + 1) % WM_KEYBUF_SIZE;
    return (int)c;
}
