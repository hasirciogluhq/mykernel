#pragma once

#include <user/gx.h>
#include <user/sdk/color.hpp>

namespace hsrc::sdk {

struct ScreenInfo {
    uint32_t width  = 0;
    uint32_t height = 0;
    uint32_t bpp    = 0;
};

struct Rect {
    int32_t x = 0, y = 0, w = 0, h = 0;
};

struct Input {
    int32_t mouse_x = 0;
    int32_t mouse_y = 0;
    uint8_t buttons = 0;   /* UGX_BTN_* */
    uint8_t mods    = 0;   /* KBD_MOD_* */
    int32_t focus_id = -1;
    int32_t wheel   = 0;   /* notches since last input(); +up / -down (PS/2) */
};

struct WindowOptions {
    int32_t x = 0;
    int32_t y = 0;
    int32_t w = 640;
    int32_t h = 480;
    int32_t min_w = 0;
    int32_t min_h = 0;
    int32_t max_w = 0;
    int32_t max_h = 0;
    int32_t radius = 0;
    uint8_t opacity = 255;
    char title[64]{};
    char class_name[32]{};
    int32_t owner_id = -1;
    int32_t parent_id = -1;

    bool acrylic = false;
    bool rounded = false;
    bool alpha = false;
    bool background = false;
    bool no_drag = false;
    bool no_title = false;
    bool topmost = false;
    bool always_on_bottom = false;
    bool resizable = true;
    bool fullscreen = false;
    bool framed = true;
    bool shadow = false;

    bool visible = true;
    bool minimized = false;
    bool maximized = false;
    bool closable = true;
    bool can_minimize = true;
    bool can_maximize = true;
    bool accept_focus = true;
    bool modal = false;

    bool capture_keys = false;
    bool capture_mouse = false;
    bool mouse_passthrough = false;

    WindowOptions();
    void set_title(const char *s);
    void set_class_name(const char *s);
};

/* Client-drawn traffic-light chrome (matches WM_TITLEBAR_H). */
constexpr int kChromeTitleH = 28;
constexpr int kChromeBtn = 12;
constexpr int kChromeBtnY = 8;
constexpr int kChromeBtn0X = 10;
constexpr int kChromeBtnGap = 8;

/* Bitmap font metrics (must match UGX_FONT_H / UGX_FONT_W in ugx_font.inc). */
constexpr int kUIFontH = 18;
constexpr int kUIFontW = 16;
constexpr int kUIFontLeading = 4;
constexpr int kUIFontLineStep = kUIFontH + kUIFontLeading;
constexpr int kUIPanelPadY = 12;
constexpr int kUISectionGap = 8;

/* Stacked panel header lines below window chrome (line 0 = first row). */
constexpr int ui_panel_text_y(int line = 0)
{
    return kChromeTitleH + kUIPanelPadY + line * kUIFontLineStep;
}

/* Y where scrollable body starts after `header_lines` of panel text. */
constexpr int ui_panel_body_top(int header_lines = 2)
{
    if (header_lines < 1)
        header_lines = 1;
    return kChromeTitleH + kUIPanelPadY +
           (header_lines - 1) * kUIFontLineStep + kUIFontH + kUISectionGap;
}

/* Vertically center single-line text inside a fixed-height row. */
constexpr int ui_text_inset_y(int row_h, int scale = 1)
{
    const int fh = kUIFontH * scale;
    return row_h > fh ? (row_h - fh) / 2 : 0;
}

enum class ChromeHit : int8_t {
    None = 0,
    Close = 1,
    Minimize = 2,
    Maximize = 3,
};

/* Mapped window pixel buffer (kernel-backed). */
class Surface {
public:
    Surface() = default;
    explicit Surface(const ugx_map &m) : m_(m) {}

    bool valid() const { return m_.pixels != nullptr && m_.width > 0; }
    uint32_t width() const { return m_.width; }
    uint32_t height() const { return m_.height; }
    uint32_t stride() const { return m_.stride; }
    Color *pixels() { return reinterpret_cast<Color *>(m_.pixels); }
    const Color *pixels() const { return reinterpret_cast<const Color *>(m_.pixels); }
    ugx_map *raw() { return &m_; }

    void clear(Color c);
    void set(int x, int y, Color c);
    void blend(int x, int y, Color c);
    void fill(int x, int y, int w, int h, Color c);
    void fill_round(int x, int y, int w, int h, int radius, Color c);
    void rect(int x, int y, int w, int h, Color c, int thickness = 1);
    void text(int x, int y, const char *s, Color c, int scale = 1);
    void text_centered(int cx, int cy, const char *s, Color c, int scale = 1);
    static int text_width(const char *s, int scale = 1);
    static int text_height(int scale = 1);
    void draw_window_chrome(int win_w, const char *title, const WindowOptions &opts,
                            Color bar_bg, Color title_color, Color border);

private:
    ugx_map m_{};
};

class Window {
public:
    Window() = default;
    ~Window() = default; /* no atexit / static dtor — call close() if needed */

    Window(const Window &) = delete;
    Window &operator=(const Window &) = delete;

    bool create(const WindowOptions &opts);
    bool set_options(const WindowOptions &opts);
    bool get_options(WindowOptions &out) const;
    bool close();
    void destroy();

    int id() const { return id_; }
    bool ok() const { return id_ >= 0 && surf_.valid(); }
    Surface &surface() { return surf_; }
    const Surface &surface() const { return surf_; }

    void fill(int x, int y, int w, int h, Color c);
    void fill_round(int x, int y, int w, int h, int radius, Color c);
    void show(bool visible);
    void hide();
    void focus();
    bool minimize();
    bool maximize();
    bool restore();
    bool toggle_maximize();
    bool set_fullscreen(bool enabled);
    bool toggle_fullscreen();
    ChromeHit hit_chrome(int lx, int ly, const WindowOptions &opts) const;
    /* Close → close()+exit(0); green escalates maximize→fullscreen. Never returns on Close. */
    bool handle_chrome_hit(ChromeHit hit);
    void damage();
    /* Window-local partial damage (compose only this rect). */
    void damage(int x, int y, int w, int h);

private:
    bool remap_surface();

    int id_ = -1;
    Surface surf_;
};

bool screen_info(ScreenInfo &out);
/* Integer UI scale from screen size (1× @ ~720p, 2× @ ≥1600×1080). */
int ui_scale();
/* Scale a logical pixel size by ui_scale(). */
int ui_px(int logical);
bool present();
bool set_wallpaper(const Color *pixels, uint32_t width, uint32_t height, uint32_t stride);
bool set_wallpaper_default();
bool set_wallpaper_color(Color c);
bool input(Input &out);
bool window_set(int id, const WindowOptions &opts);
bool window_get(int id, WindowOptions &out);
bool window_close(int id);
/* Find top matching window by class_name (−1 if none). Prefer focused/visible. */
int  window_find_class(const char *class_name);
void damage();

} // namespace hsrc::sdk
