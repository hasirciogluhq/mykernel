#include <gfx/window.h>
#include <gfx/accel.h>
#include <gfx/server.h>
#include <kernel/string.h>

static wm_window *slot_by_id(wm_t *wm, int id)
{
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (wm->windows[i].used && wm->windows[i].id == id)
            return &wm->windows[i];
    }
    return NULL;
}

static void apply_layer_style(wm_window *w, gx_layer *layer)
{
    layer->corner_radius = (w->style & WM_STYLE_ROUNDED) ? w->radius : 0;
    layer->opacity = 255;
    layer->blur_radius = 4;

    if (w->style & WM_STYLE_ACRYLIC) {
        layer->style = GX_LAYER_ACRYLIC;
        layer->tint = GX_RGBA(255, 255, 255, 120);
    } else if (w->style & WM_STYLE_ALPHA) {
        layer->style = GX_LAYER_ALPHA;
    } else {
        layer->style = GX_LAYER_OPAQUE;
    }
}

void wm_sync_layer(wm_t *wm, int id)
{
    wm_window *w = slot_by_id(wm, id);
    if (!w)
        return;
    gx_layer *L = gx_compositor_layer(wm->comp, w->layer_id);
    if (!L)
        return;
    L->bounds = w->frame;
    L->surface = w->surface;
    L->visible = w->visible;
    apply_layer_style(w, L);
    gx_server_mark_dirty();
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

wm_window *wm_create(wm_t *wm, const wm_create_args *args, int owner_pid)
{
    if (!wm || !args || args->w < 32 || args->h < 32)
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
    w->frame = gx_rect_make(args->x, args->y, args->w, args->h);
    w->style = args->style;
    if (args->radius > 0 || (args->style & WM_STYLE_ROUNDED)) {
        w->style |= WM_STYLE_ROUNDED;
        w->radius = args->radius > 0 ? args->radius : WM_DEFAULT_RADIUS;
    } else {
        w->radius = 0;
    }
    w->visible = 1;
    w->owner_pid = owner_pid;
    strncpy(w->title, args->title[0] ? args->title : "Window", WM_TITLE_MAX - 1);

    w->surface = gx_surface_create((uint32_t)args->w, (uint32_t)args->h);
    if (!w->surface) {
        w->used = 0;
        return NULL;
    }
    gx_surface_clear(w->surface, GX_TRANSPARENT);

    gx_layer layer;
    memset(&layer, 0, sizeof(layer));
    layer.visible = 1;
    layer.z = w->id;
    layer.bounds = w->frame;
    layer.surface = w->surface;
    apply_layer_style(w, &layer);

    w->layer_id = gx_compositor_add_layer(wm->comp, &layer);
    if (w->layer_id < 0) {
        gx_surface_destroy(w->surface);
        w->used = 0;
        return NULL;
    }

    if (!(w->style & WM_STYLE_BACKGROUND))
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

wm_window *wm_get(wm_t *wm, int id)
{
    return slot_by_id(wm, id);
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
    if (!w)
        return;
    w->frame.x = x;
    w->frame.y = y;
    wm_sync_layer(wm, id);
}

void wm_resize(wm_t *wm, int id, int32_t wdt, int32_t hgt)
{
    wm_window *w = slot_by_id(wm, id);
    if (!w || wdt < 32 || hgt < 32)
        return;

    gx_surface *ns = gx_surface_create((uint32_t)wdt, (uint32_t)hgt);
    if (!ns)
        return;

    gx_accel_blit(ns, 0, 0, w->surface);
    gx_surface_destroy(w->surface);
    w->surface = ns;
    w->frame.w = wdt;
    w->frame.h = hgt;
    wm_sync_layer(wm, id);
}

void wm_focus(wm_t *wm, int id)
{
    if (!wm)
        return;

    wm_window *w = slot_by_id(wm, id);
    if (w && (w->style & WM_STYLE_BACKGROUND))
        return;

    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (wm->windows[i].used)
            wm->windows[i].focused = (wm->windows[i].id == id);
    }
    wm->focus_id = id;
    if (w)
        gx_compositor_raise(wm->comp, w->layer_id);
    gx_server_mark_dirty();
}

void wm_show(wm_t *wm, int id, int visible)
{
    wm_window *w = slot_by_id(wm, id);
    if (!w)
        return;
    w->visible = visible;
    wm_sync_layer(wm, id);
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
        if (!w->used || !w->visible)
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

    if (w->style & WM_STYLE_BACKGROUND)
        return;

    wm_focus(wm, id);

    int can_drag = !(w->style & (WM_STYLE_NO_DRAG | WM_STYLE_NO_TITLE));
    int in_title = (y - w->frame.y) < WM_TITLEBAR_H;
    if (can_drag && in_title) {
        wm->drag_id = id;
        wm->drag_off_x = x - w->frame.x;
        wm->drag_off_y = y - w->frame.y;
    }
}

int wm_push_key(wm_t *wm, uint8_t ch)
{
    if (!wm || wm->focus_id < 0)
        return 0;
    wm_window *w = slot_by_id(wm, wm->focus_id);
    if (!w || (w->style & WM_STYLE_BACKGROUND))
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
