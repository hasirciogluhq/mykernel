#include <kernel/syscall.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/vfs.h>
#include <kernel/string.h>
#include <user/gx.h>
#include <gfx/server.h>
#include <gfx/accel.h>
#include <gfx/window.h>
#include <drivers/fb.h>
#include <drivers/mouse.h>
#include <drivers/keyboard.h>

typedef struct {
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags;
} regs_t;

void syscall_isr_handler(regs_t *r)
{
    r->eax = (uint32_t)syscall_dispatch(
        (long)r->eax,
        (long)r->ebx,
        (long)r->ecx,
        (long)r->edx,
        (long)r->esi,
        (long)r->edi);
}

static long do_exit(long code)
{
    process_exit((int)code);
    return 0;
}

static long do_read(long fd, long buf, long count)
{
    process_t *p = process_current();
    if (!p)
        return -1;
    int vfd = process_lookup_fd(p, (int)fd);
    if (vfd < 0)
        return -1;
    return (long)vfs_read(vfd, (void *)buf, (size_t)count);
}

static long do_write(long fd, long buf, long count)
{
    process_t *p = process_current();
    if (!p)
        return -1;
    int vfd = process_lookup_fd(p, (int)fd);
    if (vfd < 0)
        return -1;
    return (long)vfs_write(vfd, (const void *)buf, (size_t)count);
}

static long do_open(long path, long flags)
{
    process_t *p = process_current();
    if (!p)
        return -1;

    int vfd = vfs_open((const char *)path, (int)flags);
    if (vfd < 0)
        return -1;

    int ufd = process_alloc_fd(p, vfd);
    if (ufd < 0) {
        vfs_close(vfd);
        return -1;
    }
    return ufd;
}

static long do_close(long fd)
{
    process_t *p = process_current();
    if (!p)
        return -1;

    int vfd = process_lookup_fd(p, (int)fd);
    if (vfd < 0)
        return -1;

    if ((int)fd > STDERR_FILENO)
        vfs_close(vfd);

    process_free_fd(p, (int)fd);
    return 0;
}

static long do_getpid(void)
{
    process_t *p = process_current();
    return p ? (long)p->pid : -1;
}

static long do_yield(void)
{
    schedule();
    return 0;
}

static long do_gx_info(long outp)
{
    ugx_info *out = (ugx_info *)outp;
    gx_server *s = gx_server_get();
    if (!out || !s)
        return -1;
    out->width = s->device.fb->width;
    out->height = s->device.fb->height;
    out->bpp = s->device.fb->bpp;
    return 0;
}

static long do_gx_present(void)
{
    if (!gx_server_get())
        return -1;
    gx_server_present();
    return 0;
}

static long do_wm_create(long argp)
{
    const ugx_win_create *a = (const ugx_win_create *)argp;
    gx_server *s = gx_server_get();
    process_t *p = process_current();
    if (!a || !s || !p)
        return -1;

    wm_create_args wa;
    memset(&wa, 0, sizeof(wa));
    wa.x = a->x;
    wa.y = a->y;
    wa.w = a->w;
    wa.h = a->h;
    wa.style = a->style;
    wa.radius = a->radius;
    strncpy(wa.title, a->title, WM_TITLE_MAX - 1);

    wm_window *w = wm_create(&s->wm, &wa, p->pid);
    return w ? (long)w->id : -1;
}

static long do_wm_destroy(long id)
{
    gx_server *s = gx_server_get();
    if (!s)
        return -1;
    wm_destroy(&s->wm, (int)id);
    return 0;
}

static long do_wm_map(long id, long outp)
{
    gx_server *s = gx_server_get();
    ugx_map *out = (ugx_map *)outp;
    wm_map_info info;
    if (!s || !out)
        return -1;
    if (wm_map(&s->wm, (int)id, &info) < 0)
        return -1;
    out->pixels = info.pixels;
    out->width = info.width;
    out->height = info.height;
    out->stride = info.stride;
    return 0;
}

static long do_wm_move(long id, long x, long y)
{
    gx_server *s = gx_server_get();
    if (!s)
        return -1;
    wm_move(&s->wm, (int)id, (int32_t)x, (int32_t)y);
    return 0;
}

static long do_wm_resize(long id, long w, long h)
{
    gx_server *s = gx_server_get();
    if (!s)
        return -1;
    wm_resize(&s->wm, (int)id, (int32_t)w, (int32_t)h);
    return 0;
}

static long do_wm_focus(long id)
{
    gx_server *s = gx_server_get();
    if (!s)
        return -1;
    wm_focus(&s->wm, (int)id);
    return 0;
}

static long do_wm_show(long id, long vis)
{
    gx_server *s = gx_server_get();
    if (!s)
        return -1;
    wm_show(&s->wm, (int)id, (int)vis);
    return 0;
}

static long do_gx_fill(long argp, int rounded)
{
    const ugx_fill_args *a = (const ugx_fill_args *)argp;
    gx_server *s = gx_server_get();
    if (!a || !s)
        return -1;
    wm_window *w = wm_get(&s->wm, a->win);
    if (!w || !w->surface)
        return -1;

    gx_rect r = gx_rect_make(a->x, a->y, a->w, a->h);
    if (rounded)
        gx_accel_fill_round(w->surface, r, a->radius, a->color);
    else
        gx_accel_fill(w->surface, r, a->color);
    gx_server_mark_dirty();
    return 0;
}

static long do_gx_set_wallpaper(long argp)
{
    const ugx_wallpaper *a = (const ugx_wallpaper *)argp;
    gx_server *s = gx_server_get();
    if (!a || !a->pixels || !s || a->width == 0 || a->height == 0)
        return -1;

    if (s->wallpaper)
        gx_surface_destroy(s->wallpaper);

    uint32_t tw = a->width;
    uint32_t th = a->height;
    /* 1x1 = solid screen color */
    if (tw == 1 && th == 1) {
        tw = s->device.fb->width;
        th = s->device.fb->height;
    }

    s->wallpaper = gx_surface_create(tw, th);
    if (!s->wallpaper)
        return -1;

    if (a->width == 1 && a->height == 1) {
        gx_accel_fill(s->wallpaper,
                      gx_rect_make(0, 0, (int32_t)tw, (int32_t)th),
                      a->pixels[0]);
    } else {
        for (uint32_t y = 0; y < a->height && y < th; y++) {
            for (uint32_t x = 0; x < a->width && x < tw; x++) {
                s->wallpaper->pixels[y * s->wallpaper->stride + x] =
                    a->pixels[y * a->stride + x];
            }
        }
    }

    gx_compositor_set_wallpaper(&s->comp, s->wallpaper);
    gx_server_mark_dirty();
    return 0;
}

static long do_input_state(long outp)
{
    ugx_input_state *out = (ugx_input_state *)outp;
    gx_server *s = gx_server_get();
    const mouse_state_t *ms = mouse_get();
    if (!out || !s || !ms)
        return -1;

    out->mouse_x = ms->x;
    out->mouse_y = ms->y;
    out->buttons = ms->buttons;
    out->mods = keyboard_modifiers();
    out->focus_id = wm_focused_id(&s->wm);
    return 0;
}

static long do_wm_pop_key(long id)
{
    gx_server *s = gx_server_get();
    if (!s)
        return -1;
    return (long)wm_pop_key(&s->wm, (int)id);
}

static long do_gx_damage(void)
{
    gx_server_mark_dirty();
    return 0;
}

long syscall_dispatch(long n, long a1, long a2, long a3, long a4, long a5)
{
    (void)a4;
    (void)a5;

    switch (n) {
    case SYS_EXIT:   return do_exit(a1);
    case SYS_READ:   return do_read(a1, a2, a3);
    case SYS_WRITE:  return do_write(a1, a2, a3);
    case SYS_OPEN:   return do_open(a1, a2);
    case SYS_CLOSE:  return do_close(a1);
    case SYS_GETPID: return do_getpid();
    case SYS_YIELD:  return do_yield();
    case SYS_FORK:   return -1;

    case SYS_GX_INFO:          return do_gx_info(a1);
    case SYS_GX_PRESENT:       return do_gx_present();
    case SYS_WM_CREATE:        return do_wm_create(a1);
    case SYS_WM_DESTROY:       return do_wm_destroy(a1);
    case SYS_WM_MAP:           return do_wm_map(a1, a2);
    case SYS_WM_MOVE:          return do_wm_move(a1, a2, a3);
    case SYS_WM_RESIZE:        return do_wm_resize(a1, a2, a3);
    case SYS_WM_FOCUS:         return do_wm_focus(a1);
    case SYS_WM_SHOW:          return do_wm_show(a1, a2);
    case SYS_GX_FILL:          return do_gx_fill(a1, 0);
    case SYS_GX_FILL_ROUND:    return do_gx_fill(a1, 1);
    case SYS_GX_SET_WALLPAPER:  return do_gx_set_wallpaper(a1);
    case SYS_INPUT_STATE:      return do_input_state(a1);
    case SYS_WM_POP_KEY:       return do_wm_pop_key(a1);
    case SYS_GX_DAMAGE:        return do_gx_damage();

    default:         return -1;
    }
}

void syscall_init(void)
{
}

static long syscall0(long n)
{
    long ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n) : "memory");
    return ret;
}

static long syscall1(long n, long a1)
{
    long ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "b"(a1) : "memory");
    return ret;
}

static long syscall3(long n, long a1, long a2, long a3)
{
    long ret;
    __asm__ volatile("int $0x80"
                 : "=a"(ret)
                 : "a"(n), "b"(a1), "c"(a2), "d"(a3)
                 : "memory");
    return ret;
}

long sys_exit(int code) { return syscall1(SYS_EXIT, code); }
long sys_getpid(void)   { return syscall0(SYS_GETPID); }
long sys_yield(void)    { return syscall0(SYS_YIELD); }

long sys_read(int fd, void *buf, size_t count)
{
    return syscall3(SYS_READ, fd, (long)buf, (long)count);
}

long sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, fd, (long)buf, (long)count);
}

long sys_open(const char *path, int flags)
{
    long ret;
    __asm__ volatile("int $0x80"
                 : "=a"(ret)
                 : "a"(SYS_OPEN), "b"(path), "c"(flags)
                 : "memory");
    return ret;
}

long sys_close(int fd) { return syscall1(SYS_CLOSE, fd); }
