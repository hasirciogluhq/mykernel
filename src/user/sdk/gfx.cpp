#include <user/sdk/gfx.hpp>
#include <user/sdk/syscall.hpp>
#include <user/string.h>

namespace hsrc::sdk {
namespace {

#include "../ugx_font.inc"

static_assert(UGX_FONT_H == kUIFontH, "kUIFontH must match UGX_FONT_H");
static_assert(UGX_FONT_W == kUIFontW, "kUIFontW must match UGX_FONT_W");

/* Integer 4×4 supersample coverage for soft rounded corners (0..255). */
uint8_t round_coverage(int lx, int ly, int w, int h, int r)
{
    if (lx < 0 || ly < 0 || lx >= w || ly >= h)
        return 0;
    if (r <= 0)
        return 255;
    if (r * 2 > w)
        r = w / 2;
    if (r * 2 > h)
        r = h / 2;
    if (r <= 0)
        return 255;

    int x = lx;
    int y = ly;
    if (x >= w - r)
        x = w - 1 - x;
    if (y >= h - r)
        y = h - 1 - y;
    if (x >= r || y >= r)
        return 255;

    int hits = 0;
    int cx = r * 4 - 2;
    int cy = r * 4 - 2;
    int rr = cx * cx;
    for (int sy = 0; sy < 4; sy++) {
        for (int sx = 0; sx < 4; sx++) {
            int dx = cx - (x * 4 + sx);
            int dy = cy - (y * 4 + sy);
            if (dx * dx + dy * dy <= rr)
                hits++;
        }
    }
    return (uint8_t)((hits * 255 + 8) / 16);
}

const ugx_glyph &glyph(uint8_t ch)
{
    if (ch >= 32 && ch <= 126)
        return ugx_font[ch - 32];
    return ugx_font['?' - 32];
}

int chrome_btn_x(int index)
{
    return kChromeBtn0X + index * (kChromeBtn + kChromeBtnGap);
}

bool point_in_chrome_btn(int lx, int ly, int index)
{
    int bx = chrome_btn_x(index);
    return lx >= bx && lx < bx + kChromeBtn &&
           ly >= kChromeBtnY && ly < kChromeBtnY + kChromeBtn;
}

void copy_cstr(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0)
        return;
    dst[0] = 0;
    if (!src)
        return;
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = 0;
}

ugx_window_opts to_ugx(const WindowOptions &opts)
{
    ugx_window_opts raw{};
    raw.x = opts.x;
    raw.y = opts.y;
    raw.w = opts.w;
    raw.h = opts.h;
    raw.min_w = opts.min_w;
    raw.min_h = opts.min_h;
    raw.max_w = opts.max_w;
    raw.max_h = opts.max_h;
    raw.radius = opts.radius;
    raw.opacity = opts.opacity;
    copy_cstr(raw.title, sizeof(raw.title), opts.title);
    copy_cstr(raw.class_name, sizeof(raw.class_name), opts.class_name);
    raw.owner_id = opts.owner_id;
    raw.parent_id = opts.parent_id;
    raw.acrylic = opts.acrylic ? 1 : 0;
    raw.rounded = opts.rounded ? 1 : 0;
    raw.alpha = opts.alpha ? 1 : 0;
    raw.background = opts.background ? 1 : 0;
    raw.no_drag = opts.no_drag ? 1 : 0;
    raw.no_title = opts.no_title ? 1 : 0;
    raw.topmost = opts.topmost ? 1 : 0;
    raw.always_on_bottom = opts.always_on_bottom ? 1 : 0;
    raw.resizable = opts.resizable ? 1 : 0;
    raw.fullscreen = opts.fullscreen ? 1 : 0;
    raw.framed = opts.framed ? 1 : 0;
    raw.shadow = opts.shadow ? 1 : 0;
    raw.visible = opts.visible ? 1 : 0;
    raw.minimized = opts.minimized ? 1 : 0;
    raw.maximized = opts.maximized ? 1 : 0;
    raw.closable = opts.closable ? 1 : 0;
    raw.can_minimize = opts.can_minimize ? 1 : 0;
    raw.can_maximize = opts.can_maximize ? 1 : 0;
    raw.accept_focus = opts.accept_focus ? 1 : 0;
    raw.modal = opts.modal ? 1 : 0;
    raw.capture_keys = opts.capture_keys ? 1 : 0;
    raw.capture_mouse = opts.capture_mouse ? 1 : 0;
    raw.mouse_passthrough = opts.mouse_passthrough ? 1 : 0;
    return raw;
}

void from_ugx(WindowOptions &out, const ugx_window_opts &raw)
{
    out.x = raw.x;
    out.y = raw.y;
    out.w = raw.w;
    out.h = raw.h;
    out.min_w = raw.min_w;
    out.min_h = raw.min_h;
    out.max_w = raw.max_w;
    out.max_h = raw.max_h;
    out.radius = raw.radius;
    out.opacity = raw.opacity;
    copy_cstr(out.title, sizeof(out.title), raw.title);
    copy_cstr(out.class_name, sizeof(out.class_name), raw.class_name);
    out.owner_id = raw.owner_id;
    out.parent_id = raw.parent_id;
    out.acrylic = raw.acrylic != 0;
    out.rounded = raw.rounded != 0;
    out.alpha = raw.alpha != 0;
    out.background = raw.background != 0;
    out.no_drag = raw.no_drag != 0;
    out.no_title = raw.no_title != 0;
    out.topmost = raw.topmost != 0;
    out.always_on_bottom = raw.always_on_bottom != 0;
    out.resizable = raw.resizable != 0;
    out.fullscreen = raw.fullscreen != 0;
    out.framed = raw.framed != 0;
    out.shadow = raw.shadow != 0;
    out.visible = raw.visible != 0;
    out.minimized = raw.minimized != 0;
    out.maximized = raw.maximized != 0;
    out.closable = raw.closable != 0;
    out.can_minimize = raw.can_minimize != 0;
    out.can_maximize = raw.can_maximize != 0;
    out.accept_focus = raw.accept_focus != 0;
    out.modal = raw.modal != 0;
    out.capture_keys = raw.capture_keys != 0;
    out.capture_mouse = raw.capture_mouse != 0;
    out.mouse_passthrough = raw.mouse_passthrough != 0;
}

} // namespace

WindowOptions::WindowOptions()
{
    title[0] = 0;
    class_name[0] = 0;
}

void WindowOptions::set_title(const char *s)
{
    copy_cstr(title, sizeof(title), s);
}

void WindowOptions::set_class_name(const char *s)
{
    copy_cstr(class_name, sizeof(class_name), s);
}

void Surface::clear(Color c)
{
    if (!valid())
        return;
    Color *p = pixels();
    const uint32_t w = stride();
    const uint32_t h = height();
    if (w == 0 || h == 0)
        return;
    /* Fill first row, then memcpy-replicate (cache-friendly). */
    for (uint32_t x = 0; x < w; x++)
        p[x] = c;
    for (uint32_t y = 1; y < h; y++)
        memcpy(p + y * w, p, (size_t)w * sizeof(Color));
}

void Surface::set(int x, int y, Color c)
{
    if (!valid() || x < 0 || y < 0 ||
        (uint32_t)x >= width() || (uint32_t)y >= height())
        return;
    pixels()[(uint32_t)y * stride() + (uint32_t)x] = c;
}

void Surface::blend(int x, int y, Color c)
{
    if (!valid() || x < 0 || y < 0 ||
        (uint32_t)x >= width() || (uint32_t)y >= height())
        return;
    if (color_a(c) == 0)
        return;
    Color *p = &pixels()[(uint32_t)y * stride() + (uint32_t)x];
    if (color_a(c) == 255) {
        *p = c;
        return;
    }
    /* Transparent dst: keep straight RGB + coverage alpha for the compositor. */
    if (color_a(*p) == 0) {
        *p = c;
        return;
    }
    *p = color_blend(*p, c);
}

void Surface::fill(int x, int y, int w, int h, Color c)
{
    if (!valid() || w <= 0 || h <= 0)
        return;

    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w;
    int y1 = y + h;
    if (x1 > (int)width())
        x1 = (int)width();
    if (y1 > (int)height())
        y1 = (int)height();
    if (x0 >= x1 || y0 >= y1)
        return;

    Color *base = pixels();
    const uint32_t st = stride();
    const int opaque = color_a(c) == 255;

    for (int yy = y0; yy < y1; yy++) {
        Color *row = base + (uint32_t)yy * st;
        if (opaque) {
            for (int xx = x0; xx < x1; xx++)
                row[xx] = c;
        } else {
            for (int xx = x0; xx < x1; xx++) {
                if (color_a(row[xx]) == 0)
                    row[xx] = c;
                else
                    row[xx] = color_blend(row[xx], c);
            }
        }
    }
}

void Surface::fill_round(int x, int y, int w, int h, int radius, Color c)
{
    if (!valid() || w <= 0 || h <= 0)
        return;
    if (radius <= 0) {
        fill(x, y, w, h, c);
        return;
    }

    int r = radius;
    if (r * 2 > w)
        r = w / 2;
    if (r * 2 > h)
        r = h / 2;
    if (r <= 0) {
        fill(x, y, w, h, c);
        return;
    }

    /* Straight regions via fast fill; only corner tiles need AA coverage. */
    if (w > 2 * r)
        fill(x + r, y, w - 2 * r, h, c);
    if (h > 2 * r) {
        fill(x, y + r, r, h - 2 * r, c);
        fill(x + w - r, y + r, r, h - 2 * r, c);
    }

    for (int ly = 0; ly < r; ly++) {
        for (int lx = 0; lx < r; lx++) {
            uint8_t cov = round_coverage(lx, ly, w, h, r);
            if (cov == 0)
                continue;
            if (cov == 255 && color_a(c) == 255)
                set(x + lx, y + ly, c);
            else
                blend(x + lx, y + ly, color_mul_alpha(c, cov));
        }
        for (int lx = w - r; lx < w; lx++) {
            uint8_t cov = round_coverage(lx, ly, w, h, r);
            if (cov == 0)
                continue;
            if (cov == 255 && color_a(c) == 255)
                set(x + lx, y + ly, c);
            else
                blend(x + lx, y + ly, color_mul_alpha(c, cov));
        }
    }
    for (int ly = h - r; ly < h; ly++) {
        for (int lx = 0; lx < r; lx++) {
            uint8_t cov = round_coverage(lx, ly, w, h, r);
            if (cov == 0)
                continue;
            if (cov == 255 && color_a(c) == 255)
                set(x + lx, y + ly, c);
            else
                blend(x + lx, y + ly, color_mul_alpha(c, cov));
        }
        for (int lx = w - r; lx < w; lx++) {
            uint8_t cov = round_coverage(lx, ly, w, h, r);
            if (cov == 0)
                continue;
            if (cov == 255 && color_a(c) == 255)
                set(x + lx, y + ly, c);
            else
                blend(x + lx, y + ly, color_mul_alpha(c, cov));
        }
    }
}

void Surface::rect(int x, int y, int w, int h, Color c, int thickness)
{
    if (thickness < 1)
        thickness = 1;
    fill(x, y, w, thickness, c);
    fill(x, y + h - thickness, w, thickness, c);
    fill(x, y, thickness, h, c);
    fill(x + w - thickness, y, thickness, h, c);
}

int Surface::text_width(const char *s, int scale)
{
    if (!s || scale < 1)
        return 0;
    int line = 0;
    int max_w = 0;
    while (*s) {
        uint8_t ch = (uint8_t)*s++;
        if (ch == '\n') {
            if (line > max_w)
                max_w = line;
            line = 0;
            continue;
        }
        line += (int)glyph(ch).advance * scale;
    }
    if (line > max_w)
        max_w = line;
    return max_w;
}

int Surface::text_height(int scale)
{
    if (scale < 1)
        scale = 1;
    return kUIFontH * scale;
}

/* Premultiplied glyph atlas: (ch, color) → UGX_FONT_W×H tinted coverage. */
struct GlyphAtlasEntry {
    uint8_t ch = 0;
    Color   color = 0;
    Color   px[UGX_FONT_H * UGX_FONT_W]{};
    bool    valid = false;
};

constexpr int kGlyphAtlasSize = 128;
GlyphAtlasEntry g_glyph_atlas[kGlyphAtlasSize];

const Color *glyph_atlas_lookup(uint8_t ch, Color c)
{
    if (ch < 32 || ch > 126)
        ch = (uint8_t)'?';
    GlyphAtlasEntry &e = g_glyph_atlas[ch % kGlyphAtlasSize];
    if (e.valid && e.ch == ch && e.color == c)
        return e.px;

    const ugx_glyph &g = glyph(ch);
    for (int row = 0; row < UGX_FONT_H; row++) {
        for (int col = 0; col < UGX_FONT_W; col++) {
            uint8_t cov = g.alpha[row][col];
            e.px[row * UGX_FONT_W + col] =
                cov ? color_mul_alpha(c, cov) : (Color)0;
        }
    }
    e.ch = ch;
    e.color = c;
    e.valid = true;
    return e.px;
}

void Surface::text(int x, int y, const char *s, Color c, int scale)
{
    if (!s || scale < 1)
        return;
    int cx = x;
    int cy = y;
    while (*s) {
        uint8_t ch = (uint8_t)*s++;
        if (ch == '\n') {
            cx = x;
            cy += UGX_FONT_H * scale;
            continue;
        }
        const ugx_glyph &g = glyph(ch);
        if (scale == 1) {
            const Color *atlas = glyph_atlas_lookup(ch, c);
            for (int row = 0; row < UGX_FONT_H; row++) {
                for (int col = 0; col < UGX_FONT_W; col++) {
                    Color px = atlas[row * UGX_FONT_W + col];
                    if (color_a(px) == 0)
                        continue;
                    if (color_a(px) == 255)
                        set(cx + col, cy + row, px);
                    else
                        blend(cx + col, cy + row, px);
                }
            }
        } else {
            for (int row = 0; row < UGX_FONT_H; row++) {
                for (int col = 0; col < UGX_FONT_W; col++) {
                    uint8_t cov = g.alpha[row][col];
                    if (cov == 0)
                        continue;
                    Color px = color_mul_alpha(c, cov);
                    for (int sy = 0; sy < scale; sy++)
                        for (int sx = 0; sx < scale; sx++)
                            blend(cx + col * scale + sx, cy + row * scale + sy, px);
                }
            }
        }
        cx += (int)g.advance * scale;
    }
}

void Surface::text_centered(int cx, int cy, const char *s, Color c, int scale)
{
    if (!s || scale < 1)
        return;
    int tw = text_width(s, scale);
    int th = text_height(scale);
    text(cx - tw / 2, cy - th / 2, s, c, scale);
}

/* Fast opaque disc for traffic lights — no 4×4 AA (chrome redraw hot path). */
void chrome_btn_disc(Surface &s, int x, int y, int size, Color c)
{
    const int r = size / 2;
    const int rr = r * r;
    for (int ly = 0; ly < size; ly++) {
        const int dy = ly - r;
        for (int lx = 0; lx < size; lx++) {
            const int dx = lx - r;
            if (dx * dx + dy * dy <= rr)
                s.set(x + lx, y + ly, c);
        }
    }
}

void Surface::draw_window_chrome(int win_w, const char *title, const WindowOptions &opts,
                                 Color bar_bg, Color title_color, Color border)
{
    fill(0, 0, win_w, kChromeTitleH, bar_bg);
    fill(0, kChromeTitleH - 1, win_w, 1, border);

    if (opts.closable)
        chrome_btn_disc(*this, chrome_btn_x(0), kChromeBtnY, kChromeBtn, rgb(255, 95, 87));
    if (opts.can_minimize)
        chrome_btn_disc(*this, chrome_btn_x(1), kChromeBtnY, kChromeBtn, rgb(255, 189, 46));
    if (opts.can_maximize)
        chrome_btn_disc(*this, chrome_btn_x(2), kChromeBtnY, kChromeBtn, rgb(40, 200, 64));

    if (title && title[0]) {
        int tx = chrome_btn_x(3) + 4;
        if (tx < 78)
            tx = 78;
        int ty = (kChromeTitleH - UGX_FONT_H) / 2;
        if (ty < 0)
            ty = 0;
        text(tx, ty, title, title_color, 1);
    }
}

bool Window::remap_surface()
{
    if (id_ < 0)
        return false;

    ugx_map map{};
    if (syscall2(SYS_WM_MAP, id_, (long)&map) < 0) {
        return false;
    }
    surf_ = Surface(map);
    return surf_.valid();
}

bool Window::create(const WindowOptions &opts)
{
    destroy();

    ugx_window_opts raw = to_ugx(opts);
    id_ = (int)syscall1(SYS_WM_CREATE, (long)&raw);
    if (id_ < 0)
        return false;
    if (remap_surface())
        return true;
    (void)syscall1(SYS_WM_CLOSE, id_);
    id_ = -1;
    return false;
}

bool Window::set_options(const WindowOptions &opts)
{
    if (id_ < 0)
        return false;
    const uint32_t old_w = surf_.valid() ? surf_.width() : 0;
    const uint32_t old_h = surf_.valid() ? surf_.height() : 0;
    ugx_window_opts raw = to_ugx(opts);
    if (syscall2(SYS_WM_SET, id_, (long)&raw) < 0)
        return false;
    /* Minimize/show/focus flags keep the same buffer — skip WM_MAP. */
    if (surf_.valid() && (uint32_t)opts.w == old_w && (uint32_t)opts.h == old_h)
        return true;
    return remap_surface();
}

bool Window::get_options(WindowOptions &out) const
{
    if (id_ < 0)
        return false;
    ugx_window_opts raw{};
    if (syscall2(SYS_WM_GET, id_, (long)&raw) < 0)
        return false;
    from_ugx(out, raw);
    return true;
}

bool Window::close()
{
    int id = id_;
    /* Always clear local state — CRT-less globals may start as id_=0 (BSS),
     * and a failed WM_CLOSE must not leave a stale id that blocks recreate. */
    id_ = -1;
    surf_ = Surface();
    if (id < 0)
        return true;
    return syscall1(SYS_WM_CLOSE, id) == 0;
}

void Window::destroy()
{
    (void)close();
}

void Window::fill(int x, int y, int w, int h, Color c)
{
    /* Userspace mapped surface — avoid per-primitive kernel round-trips (S01/P01). */
    if (surf_.valid())
        surf_.fill(x, y, w, h, c);
}

void Window::fill_round(int x, int y, int w, int h, int radius, Color c)
{
    if (surf_.valid())
        surf_.fill_round(x, y, w, h, radius, c);
}

void Window::show(bool visible)
{
    WindowOptions opts;
    if (!get_options(opts))
        return;
    opts.visible = visible;
    if (visible)
        opts.minimized = false;
    (void)set_options(opts);
}

void Window::hide()
{
    show(false);
}

void Window::focus()
{
    (void)syscall1(SYS_WM_FOCUS, id_);
}

bool Window::minimize()
{
    WindowOptions opts;
    if (!get_options(opts))
        return false;
    opts.minimized = true;
    opts.visible = true;
    return set_options(opts);
}

bool Window::maximize()
{
    WindowOptions opts;
    if (!get_options(opts))
        return false;
    opts.maximized = true;
    opts.fullscreen = false;
    opts.minimized = false;
    opts.visible = true;
    return set_options(opts);
}

bool Window::restore()
{
    WindowOptions opts;
    if (!get_options(opts))
        return false;
    opts.maximized = false;
    opts.fullscreen = false;
    opts.minimized = false;
    opts.visible = true;
    return set_options(opts);
}

bool Window::toggle_maximize()
{
    WindowOptions opts;
    if (!get_options(opts))
        return false;
    if (opts.maximized || opts.fullscreen)
        return restore();
    return maximize();
}

bool Window::set_fullscreen(bool enabled)
{
    WindowOptions opts;
    if (!get_options(opts))
        return false;
    opts.fullscreen = enabled;
    opts.maximized = enabled ? false : opts.maximized;
    opts.minimized = false;
    opts.visible = true;
    return set_options(opts);
}

bool Window::toggle_fullscreen()
{
    WindowOptions opts;
    if (!get_options(opts))
        return false;
    return set_fullscreen(!opts.fullscreen);
}

ChromeHit Window::hit_chrome(int lx, int ly, const WindowOptions &opts) const
{
    if (ly < 0 || ly >= kChromeTitleH)
        return ChromeHit::None;
    if (opts.closable && point_in_chrome_btn(lx, ly, 0))
        return ChromeHit::Close;
    if (opts.can_minimize && point_in_chrome_btn(lx, ly, 1))
        return ChromeHit::Minimize;
    if (opts.can_maximize && point_in_chrome_btn(lx, ly, 2))
        return ChromeHit::Maximize;
    return ChromeHit::None;
}

bool Window::handle_chrome_hit(ChromeHit hit)
{
    switch (hit) {
    case ChromeHit::Close:
        (void)close();
        /* Never return from mke_main — usermode has no return address (eip→junk/#UD). */
        exit(0);
    case ChromeHit::Minimize:
        return minimize();
    case ChromeHit::Maximize: {
        WindowOptions opts;
        if (!get_options(opts))
            return false;
        if (opts.fullscreen)
            return set_fullscreen(false);
        if (opts.maximized)
            return set_fullscreen(true);
        return maximize();
    }
    case ChromeHit::None:
    default:
        return false;
    }
}

void Window::damage()
{
    (void)syscall1(SYS_GX_DAMAGE, id_);
}

void Window::damage(int x, int y, int w, int h)
{
    ugx_damage_args r;
    if (id_ < 0 || w <= 0 || h <= 0)
        return;
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    if (syscall2(SYS_GX_DAMAGE_RECT, id_, (long)&r) < 0)
        (void)syscall1(SYS_GX_DAMAGE, id_);
}

bool screen_info(ScreenInfo &out)
{
    ugx_info info{};
    if (syscall1(SYS_GX_INFO, (long)&info) < 0)
        return false;
    out.width = info.width;
    out.height = info.height;
    out.bpp = info.bpp;
    return true;
}

int ui_scale()
{
    ScreenInfo si;
    if (!screen_info(si))
        return 1;
    /* BGA default is 1280×720 → 1×. Bump at roughly 1080p+/retina-ish. */
    if (si.width >= 1600 && si.height >= 1000)
        return 2;
    return 1;
}

int ui_px(int logical)
{
    if (logical <= 0)
        return 0;
    return logical * ui_scale();
}

bool GxDevice::create(Window &w)
{
    if (!w.ok())
        return false;
    win_ = &w;
    wait_win_id_ = w.id();
    ready_ = 1;
    in_scene_ = 0;
    scene_ready_ = 0;
    last_drag_id_ = -1;
    seq_ = 0;
    chrome_bar_ = rgb(45, 45, 48);
    chrome_title_ = rgb(240, 240, 240);
    chrome_border_ = rgb(60, 60, 64);
    chrome_set_ = 1;
    for (int i = 0; i < 32; i++) {
        keys_prev_[i] = 0;
        keys_cur_[i] = 0;
    }
    return true;
}

bool GxDevice::create_shell()
{
    win_ = nullptr;
    wait_win_id_ = -1;
    ready_ = 1;
    in_scene_ = 0;
    scene_ready_ = 0;
    last_drag_id_ = -1;
    seq_ = 0;
    chrome_set_ = 0;
    for (int i = 0; i < 32; i++) {
        keys_prev_[i] = 0;
        keys_cur_[i] = 0;
    }
    return true;
}

void GxDevice::release()
{
    win_ = nullptr;
    ready_ = 0;
    in_scene_ = 0;
    scene_ready_ = 0;
}

Surface &GxDevice::backbuffer()
{
    static Surface empty;
    /* Draw target = window back (published to front on Present). */
    return win_ ? win_->surface() : empty;
}

const Surface &GxDevice::backbuffer() const
{
    static const Surface empty;
    return win_ ? win_->surface() : empty;
}

int GxDevice::client_top() const
{
    if (!win_)
        return 0;
    WindowOptions o;
    if (!win_->get_options(o))
        return kChromeTitleH;
    if (!o.framed || o.no_title)
        return 0;
    return kChromeTitleH;
}

int GxDevice::client_height() const
{
    if (!backbuffer().valid())
        return 0;
    int h = (int)backbuffer().height() - client_top();
    return h > 0 ? h : 0;
}

void GxDevice::set_chrome_colors(Color bar, Color title, Color border)
{
    chrome_bar_ = bar;
    chrome_title_ = title;
    chrome_border_ = border;
    chrome_set_ = 1;
}

bool GxDevice::dragging() const
{
    return win_ && last_drag_id_ == win_->id();
}

bool GxDevice::begin_scene()
{
    if (!ready_)
        return false;
    in_scene_ = 1;
    scene_ready_ = 0;
    return true;
}

bool GxDevice::end_scene()
{
    if (!ready_ || !in_scene_)
        return false;
    in_scene_ = 0;
    scene_ready_ = 1;
    return true;
}

bool GxDevice::present()
{
    if (!ready_ || !scene_ready_)
        return true;
    scene_ready_ = 0;

    if (win_ && win_->id() >= 0) {
        ugx_present_args a{};
        a.win = win_->id();
        a.chrome_bar = chrome_bar_;
        a.chrome_title = chrome_title_;
        a.chrome_border = chrome_border_;
        a.chrome_set = chrome_set_ ? 1 : 0;
        return syscall1(SYS_GX_PRESENT, (long)&a) == 0;
    }
    /* Shell: windows already published via Window::damage. */
    return syscall1(SYS_GX_PRESENT, 0) == 0;
}

void GxDevice::clear(Color c)
{
    int top = client_top();
    Surface &s = backbuffer();
    if (!s.valid())
        return;
    if (top <= 0)
        s.clear(c);
    else
        s.fill(0, top, (int)s.width(), (int)s.height() - top, c);
}

void GxDevice::draw_fill(int x, int y, int w, int h, Color c)
{
    backbuffer().fill(x, map_y(y), w, h, c);
}

void GxDevice::draw_fill_round(int x, int y, int w, int h, int radius, Color c)
{
    backbuffer().fill_round(x, map_y(y), w, h, radius, c);
}

void GxDevice::draw_rect(int x, int y, int w, int h, Color c, int thickness)
{
    backbuffer().rect(x, map_y(y), w, h, c, thickness);
}

void GxDevice::draw_text(int x, int y, const char *s, Color c, int scale)
{
    backbuffer().text(x, map_y(y), s, c, scale);
}

Input GxDevice::wait(uint32_t timeout_ticks)
{
    Input in{};
    if (!ready_)
        return in;
    long to = (timeout_ticks == kGxWaitForever) ? (long)-1 : (long)timeout_ticks;
    long r = syscall3(SYS_INPUT_WAIT, (long)wait_win_id_, (long)seq_, to);
    if (r >= 0)
        seq_ = (uint32_t)r;
    if (!input(in))
        return Input{};
    seq_ = in.seq;
    last_drag_id_ = in.drag_id;
    for (int i = 0; i < 32; i++) {
        keys_prev_[i] = keys_cur_[i];
        keys_cur_[i] = in.keys[i];
    }

    /* WM chrome buttons — handled here so apps stay client-only. */
    if (win_ && win_->ok() && in.hits(win_->id())) {
        WindowOptions o;
        if (win_->get_options(o) && !o.minimized && o.visible) {
            uint8_t edge = (uint8_t)(in.buttons & ~prev_buttons_);
            if (edge & UGX_BTN_LEFT) {
                int lx = in.mouse_x - o.x;
                int ly = in.mouse_y - o.y;
                ChromeHit hit = win_->hit_chrome(lx, ly, o);
                if (hit != ChromeHit::None)
                    (void)win_->handle_chrome_hit(hit);
            }
        }
    }
    prev_buttons_ = in.buttons;
    return in;
}

bool GxDevice::key_down(int vk) const
{
    if (vk < 0 || vk > 255)
        return false;
    return (keys_cur_[vk >> 3] & (uint8_t)(1u << (vk & 7))) != 0;
}

bool GxDevice::key_pressed(int vk) const
{
    if (vk < 0 || vk > 255)
        return false;
    uint8_t bit = (uint8_t)(1u << (vk & 7));
    int i = vk >> 3;
    return (keys_cur_[i] & bit) != 0 && (keys_prev_[i] & bit) == 0;
}

bool GxDevice::key_released(int vk) const
{
    if (vk < 0 || vk > 255)
        return false;
    uint8_t bit = (uint8_t)(1u << (vk & 7));
    int i = vk >> 3;
    return (keys_cur_[i] & bit) == 0 && (keys_prev_[i] & bit) != 0;
}

bool set_wallpaper(const Color *pixels, uint32_t width, uint32_t height, uint32_t stride)
{
    ugx_wallpaper args{};
    args.pixels = pixels;
    args.width = width;
    args.height = height;
    args.stride = stride;
    return syscall1(SYS_GX_SET_WALLPAPER, (long)&args) == 0;
}

bool set_wallpaper_default()
{
    return set_wallpaper(nullptr, 0, 0, 0);
}

bool set_wallpaper_color(Color c)
{
    return set_wallpaper(&c, 1, 1, 1);
}

bool input(Input &out)
{
    ugx_input_state st{};
    if (syscall1(SYS_INPUT_STATE, (long)&st) < 0)
        return false;
    out.mouse_x = st.mouse_x;
    out.mouse_y = st.mouse_y;
    out.buttons = st.buttons;
    out.mods = st.mods;
    out.focus_id = st.focus_id;
    out.hit_id = st.hit_id;
    out.wheel = st.wheel;
    out.seq = st.seq;
    out.drag_id = st.drag_id;
    for (int i = 0; i < 32; i++)
        out.keys[i] = st.keys[i];
    return true;
}

bool window_set(int id, const WindowOptions &opts)
{
    ugx_window_opts raw = to_ugx(opts);
    return syscall2(SYS_WM_SET, id, (long)&raw) == 0;
}

bool window_get(int id, WindowOptions &out)
{
    ugx_window_opts raw{};
    if (syscall2(SYS_WM_GET, id, (long)&raw) < 0)
        return false;
    from_ugx(out, raw);
    return true;
}

bool window_close(int id)
{
    return syscall1(SYS_WM_CLOSE, id) == 0;
}

int window_find_class(const char *class_name)
{
    if (!class_name || !class_name[0])
        return -1;
    long id = syscall1(SYS_WM_FIND_CLASS, (long)class_name);
    return (id >= 0) ? (int)id : -1;
}

} // namespace hsrc::sdk
