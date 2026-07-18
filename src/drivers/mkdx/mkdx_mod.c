#include "mkdx.h"
#include "console.h"
#include <kernel/initrd.h>
#include <kernel/initrd_store.h>
#include <kernel/mkdx_api.h>
#include <kernel/string.h>
#include <kernel/sync.h>
#include <user/gx.h>
#include <drivers/display.h>
#include <drivers/driver.h>
#include <drivers/mouse.h>
#include <drivers/keyboard.h>
#include <drivers/serial.h>

static mkdx_gpu g_gpu;

static uint16_t rd16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int32_t rd32les(const uint8_t *p)
{
    return (int32_t)rd32le(p);
}

static const uint8_t *initrd_find_blob(const char *name, size_t *blob_size)
{
    size_t size = 0;
    const initrd_header_t *hdr = (const initrd_header_t *)initrd_store_get(&size);
    size_t table_bytes;

    if (blob_size)
        *blob_size = 0;
    if (!hdr || size < sizeof(uint32_t) * 2 || hdr->magic != INITRD_MAGIC ||
        hdr->count == 0 || hdr->count > INITRD_MAX_FILES)
        return NULL;

    table_bytes = sizeof(uint32_t) * 2 + (size_t)hdr->count * sizeof(initrd_file_t);
    if (size < table_bytes)
        return NULL;

    for (uint32_t i = 0; i < hdr->count; i++) {
        const initrd_file_t *f = &hdr->files[i];
        if (f->size == 0 || f->offset > size || f->size > size - f->offset)
            continue;
        if (strcmp(f->name, name) != 0)
            continue;
        if (blob_size)
            *blob_size = f->size;
        return (const uint8_t *)hdr + f->offset;
    }
    return NULL;
}

static gx_surface *decode_bmp_cover(const uint8_t *bmp, size_t size,
                                    uint32_t dst_w, uint32_t dst_h)
{
    uint32_t pixel_off, dib_size, compression, row_bytes, vis_w, vis_h, crop_x, crop_y;
    int32_t src_w_s, src_h_s;
    uint32_t src_w, src_h;
    uint16_t bpp, planes;
    int top_down;
    gx_surface *dst;

    if (!bmp || size < 54 || dst_w == 0 || dst_h == 0)
        return NULL;
    if (bmp[0] != 'B' || bmp[1] != 'M')
        return NULL;

    pixel_off = rd32le(bmp + 10);
    dib_size = rd32le(bmp + 14);
    src_w_s = rd32les(bmp + 18);
    src_h_s = rd32les(bmp + 22);
    planes = rd16le(bmp + 26);
    bpp = rd16le(bmp + 28);
    compression = rd32le(bmp + 30);

    if (dib_size < 40 || src_w_s <= 0 || src_h_s == 0 || planes != 1 || compression != 0)
        return NULL;
    if (bpp != 24 && bpp != 32)
        return NULL;

    top_down = src_h_s < 0;
    src_w = (uint32_t)src_w_s;
    src_h = (uint32_t)(top_down ? -src_h_s : src_h_s);
    if (src_w == 0 || src_h == 0)
        return NULL;

    row_bytes = ((src_w * (uint32_t)bpp + 31u) / 32u) * 4u;
    if (pixel_off >= size || row_bytes == 0)
        return NULL;
    if ((uint64_t)row_bytes * (uint64_t)src_h > (uint64_t)(size - pixel_off))
        return NULL;

    if ((uint64_t)src_w * (uint64_t)dst_h > (uint64_t)src_h * (uint64_t)dst_w) {
        vis_h = src_h;
        vis_w = (uint32_t)(((uint64_t)src_h * (uint64_t)dst_w) / (uint64_t)dst_h);
        if (vis_w == 0 || vis_w > src_w)
            vis_w = src_w;
        crop_x = (src_w - vis_w) / 2u;
        crop_y = 0;
    } else {
        vis_w = src_w;
        vis_h = (uint32_t)(((uint64_t)src_w * (uint64_t)dst_h) / (uint64_t)dst_w);
        if (vis_h == 0 || vis_h > src_h)
            vis_h = src_h;
        crop_x = 0;
        crop_y = (src_h - vis_h) / 2u;
    }

    dst = gx_surface_create(dst_w, dst_h);
    if (!dst)
        return NULL;

    for (uint32_t y = 0; y < dst_h; y++) {
        uint32_t sy = crop_y + (uint32_t)(((uint64_t)y * (uint64_t)vis_h) / (uint64_t)dst_h);
        uint32_t src_row = top_down ? sy : (src_h - 1u - sy);
        const uint8_t *row = bmp + pixel_off + (size_t)src_row * row_bytes;
        for (uint32_t x = 0; x < dst_w; x++) {
            uint32_t sx = crop_x + (uint32_t)(((uint64_t)x * (uint64_t)vis_w) / (uint64_t)dst_w);
            const uint8_t *px = row + (size_t)sx * (size_t)(bpp / 8u);
            uint8_t b = px[0];
            uint8_t g = px[1];
            uint8_t r = px[2];
            uint8_t a = (bpp == 32) ? px[3] : 255u;
            dst->pixels[y * dst->stride + x] = GX_RGBA(r, g, b, a);
        }
    }

    return dst;
}

static gx_surface *load_default_wallpaper(gx_server *s)
{
    static const char *const kCandidates[] = {
        "wallpaper-default.bmp",
        "default-wallpaper.bmp",
        "default.bmp",
    };

    if (!s)
        return NULL;

    for (uint32_t i = 0; i < (uint32_t)(sizeof(kCandidates) / sizeof(kCandidates[0])); i++) {
        size_t blob_size = 0;
        const uint8_t *blob = initrd_find_blob(kCandidates[i], &blob_size);
        gx_surface *wallpaper;

        if (!blob)
            continue;
        wallpaper = decode_bmp_cover(blob, blob_size,
                                     s->device.mode.width, s->device.mode.height);
        if (wallpaper)
            return wallpaper;
    }
    return NULL;
}

int mkdx_get_screen_size(uint32_t *w, uint32_t *h, uint32_t *bpp)
{
    return display_get_screen_size(w, h, bpp);
}

int mkdx_present(void)
{
    if (!gx_server_get())
        return -1;
    gx_server_present();
    return 0;
}

static int api_info(uint32_t *w, uint32_t *h, uint32_t *bpp)
{
    gx_server *s = gx_server_get();
    if (!s)
        return -1;
    if (w)
        *w = s->device.mode.width;
    if (h)
        *h = s->device.mode.height;
    if (bpp)
        *bpp = s->device.mode.bpp;
    return 0;
}

static int api_present(void)
{
    return mkdx_present();
}

static void api_mark_dirty(int win_id)
{
    gx_server *s = gx_server_get();
    wm_window *w;

    if (!s)
        return;
    if (win_id > 0) {
        w = wm_get(&s->wm, win_id);
        if (w) {
            gx_server_mark_dirty_rect(w->frame);
            return;
        }
    }
    gx_server_mark_dirty();
}

static void api_mark_dirty_rect(int win_id, int32_t x, int32_t y, int32_t w, int32_t h)
{
    gx_server *s = gx_server_get();
    wm_window *win;
    gx_rect r;

    if (!s || w <= 0 || h <= 0)
        return;
    if (win_id <= 0) {
        gx_server_mark_dirty_rect(gx_rect_make(x, y, w, h));
        return;
    }
    win = wm_get(&s->wm, win_id);
    if (!win) {
        gx_server_mark_dirty();
        return;
    }
    /* Window-local → screen space; clamp to frame. */
    r = gx_rect_make(win->frame.x + x, win->frame.y + y, w, h);
    r = gx_rect_intersect(r, win->frame);
    if (!gx_rect_empty(r))
        gx_server_mark_dirty_rect(r);
}

static long api_wm_create(const void *args, uint32_t owner_pid)
{
    const ugx_window_opts *a = (const ugx_window_opts *)args;
    gx_server *s = gx_server_get();
    wm_window *w;

    if (!a || !s)
        return -1;
    w = wm_create(&s->wm, a, (int)owner_pid);
    return w ? (long)w->id : -1;
}

static int api_wm_set(int id, const void *opts)
{
    gx_server *s = gx_server_get();
    if (!s || !opts)
        return -1;
    return wm_apply_opts(&s->wm, id, (const ugx_window_opts *)opts);
}

static int api_wm_get(int id, void *out)
{
    gx_server *s = gx_server_get();
    if (!s || !out)
        return -1;
    return wm_get_opts(&s->wm, id, (ugx_window_opts *)out);
}

static int api_wm_close(int id)
{
    gx_server *s = gx_server_get();
    if (!s)
        return -1;
    return wm_close(&s->wm, id);
}

static int api_wm_destroy(int id)
{
    gx_server *s = gx_server_get();
    if (!s)
        return -1;
    wm_destroy(&s->wm, id);
    return 0;
}

static void api_wm_destroy_by_pid(int pid)
{
    gx_server *s = gx_server_get();
    if (!s)
        return;
    wm_destroy_by_pid(&s->wm, pid);
}

static int api_wm_map(int id, void *out)
{
    gx_server *s = gx_server_get();
    ugx_map *m = (ugx_map *)out;
    wm_map_info info;
    if (!s || !m)
        return -1;
    if (wm_map(&s->wm, id, &info) < 0)
        return -1;
    m->pixels = info.pixels;
    m->width = info.width;
    m->height = info.height;
    m->stride = info.stride;
    return 0;
}

static int api_wm_move(int id, int32_t x, int32_t y)
{
    gx_server *s = gx_server_get();
    if (!s)
        return -1;
    wm_move(&s->wm, id, x, y);
    return 0;
}

static int api_wm_resize(int id, int32_t w, int32_t h)
{
    gx_server *s = gx_server_get();
    if (!s)
        return -1;
    wm_resize(&s->wm, id, w, h);
    return 0;
}

static int api_wm_focus(int id)
{
    gx_server *s = gx_server_get();
    if (!s)
        return -1;
    wm_focus(&s->wm, id);
    return 0;
}

static int api_wm_show(int id, int vis)
{
    gx_server *s = gx_server_get();
    if (!s)
        return -1;
    wm_show(&s->wm, id, vis);
    return 0;
}

static int api_wm_get_frame(int id, void *out)
{
    gx_server *s = gx_server_get();
    ugx_frame *f = (ugx_frame *)out;
    wm_window *w;
    if (!s || !f)
        return -1;
    w = wm_get(&s->wm, id);
    if (!w)
        return -1;
    f->x = w->frame.x;
    f->y = w->frame.y;
    f->w = w->frame.w;
    f->h = w->frame.h;
    return 0;
}

static int api_wm_pop_key(int id)
{
    gx_server *s = gx_server_get();
    if (!s)
        return -1;
    return wm_pop_key(&s->wm, id);
}

static int api_wm_focused_id(void)
{
    gx_server *s = gx_server_get();
    if (!s)
        return -1;
    return wm_focused_id(&s->wm);
}

static int api_wm_find(const char *title)
{
    gx_server *s = gx_server_get();
    if (!s || !title)
        return -1;
    return wm_find_by_title(&s->wm, title);
}

static int api_wm_find_class(const char *class_name)
{
    gx_server *s = gx_server_get();
    if (!s || !class_name)
        return -1;
    return wm_find_by_class(&s->wm, class_name);
}

static int api_fill(const void *args, int rounded)
{
    const ugx_fill_args *a = (const ugx_fill_args *)args;
    gx_server *s = gx_server_get();
    wm_window *w;
    gx_rect r;

    if (!a || !s)
        return -1;
    w = wm_get(&s->wm, a->win);
    if (!w || !w->surface)
        return -1;

    r = gx_rect_make(a->x, a->y, a->w, a->h);
    if (rounded)
        gx_accel_fill_round(w->surface, r, a->radius, a->color);
    else
        gx_accel_fill(w->surface, r, a->color);
    gx_server_mark_dirty_rect(w->frame);
    return 0;
}

static int api_set_wallpaper(const void *args)
{
    const ugx_wallpaper *a = (const ugx_wallpaper *)args;
    gx_server *s = gx_server_get();
    gx_surface *next = NULL;

    if (!a || !s)
        return -1;

    if (!a->pixels || a->width == 0 || a->height == 0) {
        next = load_default_wallpaper(s);
    } else {
        /* Keep solid colors as 1x1 — expanding to fullscreen OOMs the bump heap. */
        next = gx_surface_create(a->width, a->height);
        if (!next)
            return -1;

        if (a->width == 1 && a->height == 1) {
            next->pixels[0] = a->pixels[0];
        } else {
            for (uint32_t y = 0; y < a->height; y++) {
                for (uint32_t x = 0; x < a->width; x++) {
                    next->pixels[y * next->stride + x] =
                    a->pixels[y * a->stride + x];
                }
            }
        }
    }

    if (!next)
        return -1;

    if (s->wallpaper)
        gx_surface_destroy(s->wallpaper);
    s->wallpaper = next;

    gx_compositor_set_wallpaper(&s->comp, s->wallpaper);
    gx_server_mark_dirty();
    return 0;
}

static int api_input_state(void *out)
{
    ugx_input_state *o = (ugx_input_state *)out;
    gx_server *s = gx_server_get();
    const mouse_state_t *ms;

    if (!o || !s)
        return -1;

    /* Apps read input before present — poll so coordinates are current. */
    gx_server_pump_input();

    ms = mouse_get();
    if (!ms)
        return -1;
    o->mouse_x = ms->x;
    o->mouse_y = ms->y;
    o->buttons = ms->buttons;
    o->mods = keyboard_modifiers();
    o->focus_id = wm_focused_id(&s->wm);
    o->hit_id = wm_hit_test(&s->wm, ms->x, ms->y);
    o->wheel = mouse_consume_wheel();
    o->seq = input_event_seq();
    keyboard_keys_bitmap(o->keys);
    o->drag_id = s->wm.drag_id;
    return 0;
}

static void api_pump_input(void)
{
    /* Input first, then independent compositor clock (C10/T01/S06). */
    gx_server_pump_input();
    (void)gx_server_frame_tick();
}

static int api_console_alloc(int pid, const char *name, int visible)
{
    gx_server *s = gx_server_get();
    if (!s)
        return -1;
    return proc_console_alloc(&s->wm, pid, name, visible);
}

static void api_console_free(int pid)
{
    proc_console_free(pid);
}

static ssize_t api_console_write(int pid, const void *buf, size_t len)
{
    return proc_console_write(pid, buf, len);
}

static int api_console_show(int pid, int visible)
{
    return proc_console_show(pid, visible);
}

static const mkdx_api_t g_api = {
    .info = api_info,
    .present = api_present,
    .mark_dirty = api_mark_dirty,
    .mark_dirty_rect = api_mark_dirty_rect,
    .wm_create = api_wm_create,
    .wm_set = api_wm_set,
    .wm_get = api_wm_get,
    .wm_close = api_wm_close,
    .wm_destroy = api_wm_destroy,
    .wm_destroy_by_pid = api_wm_destroy_by_pid,
    .wm_map = api_wm_map,
    .wm_move = api_wm_move,
    .wm_resize = api_wm_resize,
    .wm_focus = api_wm_focus,
    .wm_show = api_wm_show,
    .wm_get_frame = api_wm_get_frame,
    .wm_pop_key = api_wm_pop_key,
    .wm_focused_id = api_wm_focused_id,
    .wm_find = api_wm_find,
    .wm_find_class = api_wm_find_class,
    .fill = api_fill,
    .set_wallpaper = api_set_wallpaper,
    .input_state = api_input_state,
    .pump_input = api_pump_input,
    .console_alloc = api_console_alloc,
    .console_free = api_console_free,
    .console_write = api_console_write,
    .console_show = api_console_show,
};

static int mkdx_drv_probe(driver_t *drv, void *ctx)
{
    (void)drv;
    (void)ctx;
    return display_active() ? 0 : -1;
}

static int mkdx_drv_init(driver_t *drv, void *ctx)
{
    (void)drv;
    (void)ctx;
    klog("[mkdx] init\n");
    if (gx_server_init() < 0) {
        klog("[mkdx] gx_server_init FAILED\n");
        return -1;
    }
    if (mkdx_gpu_init(&g_gpu) < 0) {
        klog("[mkdx] mkdx_gpu_init FAILED\n");
        return -1;
    }
    mkdx_api_register(&g_api);
    klog("[mkdx] ready\n");
    return 0;
}

int kmod_init(void)
{
    driver_t d;
    memset(&d, 0, sizeof(d));
    strncpy(d.name, "mkdx", DRIVER_NAME_MAX - 1);
    strncpy(d.version, "1.0", DRIVER_VERSION_MAX - 1);
    d.kind = DRIVER_KIND_CUSTOM;
    d.class = DRIVER_CLASS_DISPLAY;
    d.flags = 0;
    d.priority = 50;
    d.probe = mkdx_drv_probe;
    d.init = mkdx_drv_init;

    if (driver_register(&d) < 0)
        return -1;
    if (driver_load("mkdx", NULL) < 0)
        return -1;
    return 0;
}
