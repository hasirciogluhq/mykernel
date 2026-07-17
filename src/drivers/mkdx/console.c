#include "console.h"
#include "accel.h"
#include "font.h"
#include "server.h"
#include <kernel/heap.h>
#include <kernel/string.h>

typedef struct proc_console {
    int   used;
    int   pid;
    int   win_id;
    int   visible;
    int   dirty;
    char  name[64];
    char  buf[PROC_CONSOLE_BUF_SIZE];
    size_t len;
    struct proc_console *free_next;
} proc_console_t;

/* Pointer slots only — 8KiB buffers allocate/recycle on demand. */
static proc_console_t *g_consoles[PROC_CONSOLE_MAX];
static proc_console_t *g_console_freelist;

static proc_console_t *slot_by_pid(int pid)
{
    for (int i = 0; i < PROC_CONSOLE_MAX; i++) {
        if (g_consoles[i] && g_consoles[i]->used && g_consoles[i]->pid == pid)
            return g_consoles[i];
    }
    return NULL;
}

static void console_push_freelist(proc_console_t *c)
{
    if (!c)
        return;
    c->used = 0;
    c->free_next = g_console_freelist;
    g_console_freelist = c;
}

static proc_console_t *console_pop_freelist(void)
{
    proc_console_t *c = g_console_freelist;
    if (!c)
        return NULL;
    g_console_freelist = c->free_next;
    c->free_next = NULL;
    return c;
}

static proc_console_t *alloc_slot(int pid)
{
    proc_console_t *c;
    int i;

    for (i = 0; i < PROC_CONSOLE_MAX; i++) {
        if (g_consoles[i])
            continue;

        c = console_pop_freelist();
        if (!c) {
            c = (proc_console_t *)kmalloc(sizeof(*c));
            if (!c)
                return NULL;
        }
        memset(c, 0, sizeof(*c));
        c->used = 1;
        c->pid = pid;
        c->win_id = -1;
        g_consoles[i] = c;
        return c;
    }
    return NULL;
}

static void append_bytes(proc_console_t *c, const char *data, size_t n)
{
    size_t i;

    if (!c || !data || n == 0)
        return;

    for (i = 0; i < n; i++) {
        if (c->len >= PROC_CONSOLE_BUF_SIZE - 1) {
            size_t keep = PROC_CONSOLE_BUF_SIZE / 2;
            memmove(c->buf, c->buf + c->len - keep, keep);
            c->len = keep;
            c->buf[c->len] = 0;
        }
        c->buf[c->len++] = data[i];
    }
    c->buf[c->len] = 0;
    c->dirty = 1;
}

static void paint_console(wm_t *wm, proc_console_t *c)
{
    wm_window *w;
    gx_surface *s;
    int32_t x0, y0, cols, rows, line, col;
    size_t i;
    gx_color bg = GX_RGB(18, 18, 22);
    gx_color fg = GX_RGB(220, 220, 220);

    if (!wm || !c || c->win_id < 0)
        return;

    w = wm_get(wm, c->win_id);
    if (!w || !w->surface)
        return;

    s = w->surface;
    gx_accel_fill(s, gx_rect_make(0, 0, (int32_t)s->width, (int32_t)s->height), bg);

    x0 = 8;
    y0 = 8;
    cols = (int32_t)s->width > 16 ? ((int32_t)s->width - 16) / 9 : PROC_CONSOLE_COLS;
    if (cols > PROC_CONSOLE_COLS)
        cols = PROC_CONSOLE_COLS;
    rows = (int32_t)s->height > 16 ? ((int32_t)s->height - 16) / PROC_CONSOLE_ROW_H : 1;
    if (rows < 1)
        rows = 1;

    line = 0;
    col = 0;
    for (i = 0; c->buf[i] && line < rows; i++) {
        char ch = c->buf[i];
        if (ch == '\r')
            continue;
        if (ch == '\n') {
            line++;
            col = 0;
            continue;
        }
        if (col >= cols) {
            line++;
            col = 0;
            if (line >= rows)
                break;
        }
        if (line >= rows)
            break;
        if ((unsigned char)ch >= 32 && (unsigned char)ch <= 126)
            gx_draw_char(s, x0 + col * 9, y0 + line * PROC_CONSOLE_ROW_H,
                         (uint8_t)ch, fg);
        col++;
    }

    wm_sync_layer(wm, c->win_id);
    c->dirty = 0;
}

static int proc_console_create_window(wm_t *wm, proc_console_t *c, int visible)
{
    ugx_window_opts opts;
    wm_window *w;

    if (!wm || !c || c->win_id >= 0)
        return 0;

    memset(&opts, 0, sizeof(opts));
    opts.x = 80 + (c->pid % 5) * 24;
    opts.y = 80 + (c->pid % 5) * 18;
    opts.w = 720;
    opts.h = 420;
    opts.radius = 10;
    opts.opacity = 255;
    strncpy(opts.class_name, "proc.console", sizeof(opts.class_name) - 1);
    if (c->name[0]) {
        size_t pos = 0;
        const char *prefix = "Console: ";

        while (prefix[pos] && pos < sizeof(opts.title) - 1) {
            opts.title[pos] = prefix[pos];
            pos++;
        }
        for (size_t ni = 0; c->name[ni] && pos < sizeof(opts.title) - 1; ni++, pos++)
            opts.title[pos] = c->name[ni];
        opts.title[pos] = 0;
    } else {
        strncpy(opts.title, "Console", sizeof(opts.title) - 1);
    }
    opts.rounded = 1;
    opts.framed = 1;
    opts.resizable = 1;
    opts.closable = 1;
    opts.can_minimize = 1;
    opts.can_maximize = 0;
    opts.accept_focus = 1;
    opts.visible = visible ? 1 : 0;

    w = wm_create(wm, &opts, c->pid);
    if (!w)
        return -1;

    c->win_id = w->id;
    c->visible = visible ? 1 : 0;
    c->dirty = 1;
    if (visible)
        paint_console(wm, c);
    return 0;
}

static int proc_console_ensure_window(wm_t *wm, proc_console_t *c, int visible)
{
    if (!c)
        return -1;
    if (c->win_id >= 0)
        return 0;
    return proc_console_create_window(wm, c, visible);
}

int proc_console_init(void)
{
    memset(g_consoles, 0, sizeof(g_consoles));
    g_console_freelist = NULL;
    return 0;
}

void proc_console_shutdown(void)
{
    gx_server *srv = gx_server_get();
    wm_t *wm = srv ? &srv->wm : NULL;

    for (int i = 0; i < PROC_CONSOLE_MAX; i++) {
        proc_console_t *c = g_consoles[i];
        if (!c || !c->used)
            continue;
        if (wm && c->win_id >= 0)
            wm_destroy(wm, c->win_id);
        g_consoles[i] = NULL;
        console_push_freelist(c);
    }
}

int proc_console_alloc(wm_t *wm, int pid, const char *name, int visible)
{
    proc_console_t *c;

    if (!wm || pid <= 0)
        return -1;
    if (slot_by_pid(pid))
        return 0;

    c = alloc_slot(pid);
    if (!c)
        return -1;

    if (name && name[0])
        strncpy(c->name, name, sizeof(c->name) - 1);
    else
        c->name[0] = 0;

    if (visible)
        return proc_console_create_window(wm, c, 1);

    c->visible = 0;
    return 0;
}

void proc_console_free(int pid)
{
    gx_server *srv = gx_server_get();
    wm_t *wm = srv ? &srv->wm : NULL;
    proc_console_t *c = slot_by_pid(pid);
    int i;

    if (!c)
        return;
    if (wm && c->win_id >= 0)
        wm_destroy(wm, c->win_id);
    for (i = 0; i < PROC_CONSOLE_MAX; i++) {
        if (g_consoles[i] == c) {
            g_consoles[i] = NULL;
            break;
        }
    }
    console_push_freelist(c);
}

ssize_t proc_console_write(int pid, const void *buf, size_t len)
{
    gx_server *srv = gx_server_get();
    wm_t *wm = srv ? &srv->wm : NULL;
    proc_console_t *c = slot_by_pid(pid);
    const char *data = (const char *)buf;

    if (!c || !data || len == 0)
        return 0;

    append_bytes(c, data, len);
    if (c->visible && wm) {
        if (proc_console_ensure_window(wm, c, 1) < 0)
            return (ssize_t)len;
        paint_console(wm, c);
    }
    return (ssize_t)len;
}

int proc_console_show(int pid, int visible)
{
    gx_server *srv = gx_server_get();
    wm_t *wm = srv ? &srv->wm : NULL;
    proc_console_t *c = slot_by_pid(pid);

    if (!c || !wm)
        return -1;

    if (visible) {
        if (proc_console_ensure_window(wm, c, 1) < 0)
            return -1;
    } else if (c->win_id < 0) {
        c->visible = 0;
        return 0;
    }

    c->visible = visible ? 1 : 0;
    wm_show(wm, c->win_id, c->visible);
    if (c->visible) {
        c->dirty = 1;
        paint_console(wm, c);
    }
    return 0;
}

void proc_console_paint_dirty(wm_t *wm)
{
    if (!wm)
        return;
    for (int i = 0; i < PROC_CONSOLE_MAX; i++) {
        proc_console_t *c = g_consoles[i];
        if (!c || !c->used || !c->dirty || !c->visible)
            continue;
        paint_console(wm, c);
    }
}
