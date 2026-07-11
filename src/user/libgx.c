#include <user/gx.h>

static long syscall1(long n, long a1)
{
    long ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "b"(a1) : "memory");
    return ret;
}

static long syscall2(long n, long a1, long a2)
{
    long ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(n), "b"(a1), "c"(a2)
                     : "memory");
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

int ugx_info_get(ugx_info *out)
{
    return (int)syscall1(SYS_GX_INFO, (long)out);
}

int ugx_present(void)
{
    return (int)syscall1(SYS_GX_PRESENT, 0);
}

int ugx_wm_create(const ugx_win_create *args)
{
    return (int)syscall1(SYS_WM_CREATE, (long)args);
}

int ugx_wm_destroy(int win)
{
    return (int)syscall1(SYS_WM_DESTROY, win);
}

int ugx_wm_map(int win, ugx_map *out)
{
    return (int)syscall2(SYS_WM_MAP, win, (long)out);
}

int ugx_wm_move(int win, int x, int y)
{
    return (int)syscall3(SYS_WM_MOVE, win, x, y);
}

int ugx_wm_resize(int win, int w, int h)
{
    return (int)syscall3(SYS_WM_RESIZE, win, w, h);
}

int ugx_wm_focus(int win)
{
    return (int)syscall1(SYS_WM_FOCUS, win);
}

int ugx_wm_show(int win, int visible)
{
    return (int)syscall2(SYS_WM_SHOW, win, visible);
}

int ugx_wm_get_frame(int win, ugx_frame *out)
{
    return (int)syscall2(SYS_WM_GET_FRAME, win, (long)out);
}

int ugx_fill(int win, int x, int y, int w, int h, uint32_t color)
{
    ugx_fill_args args;
    args.win = win;
    args.x = x;
    args.y = y;
    args.w = w;
    args.h = h;
    args.color = color;
    args.radius = 0;
    return (int)syscall1(SYS_GX_FILL, (long)&args);
}

int ugx_fill_round(int win, int x, int y, int w, int h, int radius, uint32_t color)
{
    ugx_fill_args args;
    args.win = win;
    args.x = x;
    args.y = y;
    args.w = w;
    args.h = h;
    args.color = color;
    args.radius = radius;
    return (int)syscall1(SYS_GX_FILL_ROUND, (long)&args);
}

int ugx_set_wallpaper(const uint32_t *pixels, uint32_t w, uint32_t h, uint32_t stride)
{
    ugx_wallpaper args;
    args.pixels = pixels;
    args.width = w;
    args.height = h;
    args.stride = stride;
    return (int)syscall1(SYS_GX_SET_WALLPAPER, (long)&args);
}

int ugx_input_get(ugx_input_state *out)
{
    return (int)syscall1(SYS_INPUT_STATE, (long)out);
}

int ugx_wm_pop_key(int win)
{
    return (int)syscall1(SYS_WM_POP_KEY, win);
}

int ugx_damage(void)
{
    return (int)syscall1(SYS_GX_DAMAGE, 0);
}

void ugx_buf_set(ugx_map *m, int x, int y, uint32_t c)
{
    if (!m || !m->pixels || x < 0 || y < 0 ||
        (uint32_t)x >= m->width || (uint32_t)y >= m->height)
        return;
    m->pixels[(uint32_t)y * m->stride + (uint32_t)x] = c;
}

void ugx_buf_clear(ugx_map *m, uint32_t c)
{
    if (!m || !m->pixels)
        return;
    for (uint32_t i = 0; i < m->stride * m->height; i++)
        m->pixels[i] = c;
}

void ugx_buf_fill(ugx_map *m, int x, int y, int w, int h, uint32_t c)
{
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++)
            ugx_buf_set(m, x + xx, y + yy, c);
}

void ugx_buf_rect(ugx_map *m, int x, int y, int w, int h, uint32_t c, int t)
{
    if (t < 1)
        t = 1;
    ugx_buf_fill(m, x, y, w, t, c);
    ugx_buf_fill(m, x, y + h - t, w, t, c);
    ugx_buf_fill(m, x, y, t, h, c);
    ugx_buf_fill(m, x + w - t, y, t, h, c);
}

#include "ugx_font.inc"

/* ISO-8859-9 Turkish glyphs (same bitmaps as kernel font) */
static const uint8_t ugx_tr_GBREVE[8] = {0x0C,0x00,0x1E,0x33,0x3F,0x33,0x33,0x00};
static const uint8_t ugx_tr_gbreve[8] = {0x0C,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F};
static const uint8_t ugx_tr_UDIAER[8] = {0x36,0x00,0x33,0x33,0x33,0x33,0x1E,0x00};
static const uint8_t ugx_tr_udiaer[8] = {0x36,0x00,0x33,0x33,0x33,0x33,0x6E,0x00};
static const uint8_t ugx_tr_SCEDIL[8] = {0x1E,0x33,0x03,0x03,0x33,0x1E,0x08,0x06};
static const uint8_t ugx_tr_scedil[8] = {0x00,0x00,0x1E,0x03,0x1E,0x30,0x1E,0x0C};
static const uint8_t ugx_tr_IDOT[8]   = {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00};
static const uint8_t ugx_tr_idotless[8]={0x00,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00};
static const uint8_t ugx_tr_ODIAER[8] = {0x36,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00};
static const uint8_t ugx_tr_odiaer[8] = {0x36,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00};
static const uint8_t ugx_tr_CCEDIL[8] = {0x1E,0x33,0x03,0x03,0x33,0x1E,0x08,0x06};
static const uint8_t ugx_tr_ccedil[8] = {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x0C};

static const uint8_t *ugx_glyph(uint8_t ch)
{
    if (ch >= 32 && ch <= 126)
        return ugx_font[ch - 32];
    switch (ch) {
    case 0xD0: return ugx_tr_GBREVE;
    case 0xF0: return ugx_tr_gbreve;
    case 0xDC: return ugx_tr_UDIAER;
    case 0xFC: return ugx_tr_udiaer;
    case 0xDE: return ugx_tr_SCEDIL;
    case 0xFE: return ugx_tr_scedil;
    case 0xDD: return ugx_tr_IDOT;
    case 0xFD: return ugx_tr_idotless;
    case 0xD6: return ugx_tr_ODIAER;
    case 0xF6: return ugx_tr_odiaer;
    case 0xC7: return ugx_tr_CCEDIL;
    case 0xE7: return ugx_tr_ccedil;
    default:   return ugx_font['?' - 32];
    }
}

void ugx_buf_text(ugx_map *m, int x, int y, const char *text, uint32_t c)
{
    if (!text)
        return;
    int cx = x;
    while (*text) {
        uint8_t ch = (uint8_t)*text++;
        if (ch == '\n') {
            cx = x;
            y += 10;
            continue;
        }
        const uint8_t *g = ugx_glyph(ch);
        for (int row = 0; row < 8; row++) {
            uint8_t bits = g[row];
            for (int col = 0; col < 8; col++) {
                if (bits & (1u << col))
                    ugx_buf_set(m, cx + col, y + row, c);
            }
        }
        cx += 8;
    }
}
