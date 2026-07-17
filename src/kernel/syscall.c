#include <kernel/syscall.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/vfs.h>
#include <kernel/mkdx_api.h>
#include <kernel/uaccess.h>
#include <kernel/string.h>
#include <user/gx.h>

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
    char tmp[256];
    long total = 0;
    int vfd;

    if (!p || count < 0)
        return -1;
    vfd = process_lookup_fd(p, (int)fd);
    if (vfd < 0)
        return -1;

    while (total < count) {
        size_t chunk = (size_t)(count - total);
        ssize_t n;
        if (chunk > sizeof(tmp))
            chunk = sizeof(tmp);
        n = vfs_read(vfd, tmp, chunk);
        if (n < 0)
            return total > 0 ? total : -1;
        if (n == 0)
            break;
        if (copy_to_user((void *)(buf + total), tmp, (size_t)n) < 0)
            return -1;
        total += n;
        if ((size_t)n < chunk)
            break;
    }
    return total;
}

static long do_write(long fd, long buf, long count)
{
    process_t *p = process_current();
    char tmp[256];
    long total = 0;
    int vfd;

    if (!p || count < 0)
        return -1;
    vfd = process_lookup_fd(p, (int)fd);
    if (vfd < 0)
        return -1;

    while (total < count) {
        size_t chunk = (size_t)(count - total);
        ssize_t n;
        if (chunk > sizeof(tmp))
            chunk = sizeof(tmp);
        if (copy_from_user(tmp, (const void *)(buf + total), chunk) < 0)
            return -1;
        n = vfs_write(vfd, tmp, chunk);
        if (n < 0)
            return total > 0 ? total : -1;
        total += n;
        if ((size_t)n < chunk)
            break;
    }
    return total;
}

static long do_open(long path, long flags)
{
    process_t *p = process_current();
    char kpath[256];
    int len;
    int vfd;
    int ufd;

    if (!p)
        return -1;
    len = user_strlen((const char *)path, sizeof(kpath));
    if (len < 0)
        return -1;
    if (copy_from_user(kpath, (const void *)path, (size_t)len + 1) < 0)
        return -1;

    vfd = vfs_open(kpath, (int)flags);
    if (vfd < 0)
        return -1;

    ufd = process_alloc_fd(p, vfd);
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

static const mkdx_api_t *mkdx(void)
{
    return mkdx_api_get();
}

static long do_gx_info(long outp)
{
    ugx_info info;
    const mkdx_api_t *api = mkdx();
    if (!outp || !api || !api->info)
        return -1;
    if (api->info(&info.width, &info.height, &info.bpp) < 0)
        return -1;
    if (copy_to_user((void *)outp, &info, sizeof(info)) < 0)
        return -1;
    return 0;
}

static long do_gx_present(void)
{
    const mkdx_api_t *api = mkdx();
    if (!api || !api->present)
        return -1;
    return api->present();
}

static long do_wm_create(long argp)
{
    ugx_win_create args;
    const mkdx_api_t *api = mkdx();
    process_t *p = process_current();
    if (!api || !api->wm_create)
        return -1;
    if (copy_from_user(&args, (const void *)argp, sizeof(args)) < 0)
        return -1;
    return api->wm_create(&args, p ? (uint32_t)p->pid : 0);
}

static long do_wm_destroy(long id)
{
    const mkdx_api_t *api = mkdx();
    if (!api || !api->wm_destroy)
        return -1;
    return api->wm_destroy((int)id);
}

static long do_wm_map(long id, long outp)
{
    ugx_map m;
    const mkdx_api_t *api = mkdx();
    if (!api || !api->wm_map)
        return -1;
    if (api->wm_map((int)id, &m) < 0)
        return -1;
    if (copy_to_user((void *)outp, &m, sizeof(m)) < 0)
        return -1;
    return 0;
}

static long do_wm_move(long id, long x, long y)
{
    const mkdx_api_t *api = mkdx();
    if (!api || !api->wm_move)
        return -1;
    return api->wm_move((int)id, (int32_t)x, (int32_t)y);
}

static long do_wm_resize(long id, long w, long h)
{
    const mkdx_api_t *api = mkdx();
    if (!api || !api->wm_resize)
        return -1;
    return api->wm_resize((int)id, (int32_t)w, (int32_t)h);
}

static long do_wm_focus(long id)
{
    const mkdx_api_t *api = mkdx();
    if (!api || !api->wm_focus)
        return -1;
    return api->wm_focus((int)id);
}

static long do_wm_show(long id, long vis)
{
    const mkdx_api_t *api = mkdx();
    if (!api || !api->wm_show)
        return -1;
    return api->wm_show((int)id, (int)vis);
}

static long do_gx_fill(long argp, int rounded)
{
    ugx_fill_args args;
    const mkdx_api_t *api = mkdx();
    if (!api || !api->fill)
        return -1;
    if (copy_from_user(&args, (const void *)argp, sizeof(args)) < 0)
        return -1;
    return api->fill(&args, rounded);
}

static long do_gx_set_wallpaper(long argp)
{
    ugx_wallpaper args;
    ugx_wallpaper kargs;
    uint32_t pixels[64];
    size_t npix;
    const mkdx_api_t *api = mkdx();

    if (!api || !api->set_wallpaper)
        return -1;
    if (copy_from_user(&args, (const void *)argp, sizeof(args)) < 0)
        return -1;
    if (!args.pixels || args.width == 0 || args.height == 0)
        return -1;

    npix = (size_t)args.width * (size_t)args.height;
    if (npix > sizeof(pixels) / sizeof(pixels[0]))
        return -1;
    if (copy_from_user(pixels, args.pixels, npix * sizeof(uint32_t)) < 0)
        return -1;

    kargs = args;
    kargs.pixels = pixels;
    kargs.stride = args.width;
    return api->set_wallpaper(&kargs);
}

static long do_input_state(long outp)
{
    ugx_input_state st;
    const mkdx_api_t *api = mkdx();
    if (!api || !api->input_state)
        return -1;
    if (api->input_state(&st) < 0)
        return -1;
    if (copy_to_user((void *)outp, &st, sizeof(st)) < 0)
        return -1;
    return 0;
}

static long do_wm_pop_key(long id)
{
    const mkdx_api_t *api = mkdx();
    if (!api || !api->wm_pop_key)
        return -1;
    return api->wm_pop_key((int)id);
}

static long do_gx_damage(void)
{
    const mkdx_api_t *api = mkdx();
    if (!api || !api->mark_dirty)
        return -1;
    api->mark_dirty();
    return 0;
}

static long do_wm_get_frame(long id, long outp)
{
    ugx_frame fr;
    const mkdx_api_t *api = mkdx();
    if (!api || !api->wm_get_frame)
        return -1;
    if (api->wm_get_frame((int)id, &fr) < 0)
        return -1;
    if (copy_to_user((void *)outp, &fr, sizeof(fr)) < 0)
        return -1;
    return 0;
}

static long do_wm_find(long titlep)
{
    char title[64];
    int len;
    const mkdx_api_t *api = mkdx();

    if (!api || !api->wm_find)
        return -1;
    len = user_strlen((const char *)titlep, sizeof(title));
    if (len < 0)
        return -1;
    if (copy_from_user(title, (const void *)titlep, (size_t)len + 1) < 0)
        return -1;
    return api->wm_find(title);
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
    case SYS_GX_SET_WALLPAPER: return do_gx_set_wallpaper(a1);
    case SYS_INPUT_STATE:      return do_input_state(a1);
    case SYS_WM_POP_KEY:       return do_wm_pop_key(a1);
    case SYS_GX_DAMAGE:        return do_gx_damage();
    case SYS_WM_GET_FRAME:     return do_wm_get_frame(a1, a2);
    case SYS_WM_FIND:          return do_wm_find(a1);

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
