#include <user/mke.h>
#include <kernel/syscall.h>
#include <user/sdk/fs.hpp>
#include <user/sdk/gfx.hpp>
#include <user/sdk/settings.hpp>
#include <user/sdk/syscall.hpp>
#include <user/sdk/sync.hpp>
#include <user/sdk/thread.hpp>
#include <user/sdk/time.hpp>
#include <user/string.h>
#include <drivers/keyboard.h>

/*
 * System Settings — gfx API with dropdown / toggle / slider widgets,
 * native wheel scrolling, and a content scrollbar + thumb.
 */

namespace {

using hsrc::sdk::ChromeHit;
using hsrc::sdk::Input;
using hsrc::sdk::ScreenInfo;
using hsrc::sdk::Surface;
using hsrc::sdk::GxDevice;
using hsrc::sdk::Window;
using hsrc::sdk::WindowOptions;
using hsrc::sdk::kGxWaitForever;
using hsrc::sdk::kChromeTitleH;
using hsrc::sdk::ui_panel_body_top;
using hsrc::sdk::ui_panel_text_y;
using hsrc::sdk::ui_text_inset_y;
using hsrc::sdk::settings::Appearance;
using hsrc::sdk::settings::persist_key;
using hsrc::sdk::settings::refresh_theme;
using hsrc::sdk::settings::set_appearance;
using hsrc::sdk::settings::theme;
using hsrc::sdk::settings::kThemeWaitTicks;

constexpr int kWinW = 860;
constexpr int kWinH = 520;
constexpr int kSidebarW = 200;
constexpr int kPad = 14;
constexpr int kRowH = 36;
constexpr int kRowGap = 4;
constexpr int kContentX = kSidebarW + kPad;
constexpr int kContentW = kWinW - kContentX - kPad - 18; /* leave scrollbar lane */
constexpr int kScrollX = kWinW - 16;
constexpr int kScrollW = 8;
constexpr int kSidebarRowH = 28;
constexpr int kSidebarRowStep = kSidebarRowH + 4;
constexpr int kContentTop = ui_panel_body_top(2);
constexpr int kContentBot = kWinH - 12;
constexpr int kViewH = kContentBot - kContentTop;

constexpr const char *kIniPath = "/etc/os-settings.ini";
constexpr const char *kDeepLinkPath = "/run/settings.deeplink";
constexpr size_t kDeepLinkBytes = 128;
constexpr size_t kIniBytes = 4096;

enum CategoryIndex {
    CAT_GENERAL = 0,
    CAT_APPEARANCE,
    CAT_DESKTOP_DOCK,
    CAT_MOUSE,
    CAT_DATE_TIME,
    CAT_DISPLAY,
    CAT_ABOUT,
    CAT_COUNT
};

enum WidgetKind {
    W_DROPDOWN = 0,
    W_TOGGLE = 1,
    W_SLIDER = 2,
};

enum TargetKind {
    TARGET_SETTING = 0,
    TARGET_SCROLLBAR = 1,
};

struct Category {
    const char *id;
    const char *label;
};

struct Setting {
    const char *key;
    int category;
    const char *label;
    WidgetKind kind;
    const char *const *choices;
    int choice_count;
    int min_v;
    int max_v;
    int current; /* choice index OR slider value */
};

struct ClickTarget {
    int y;
    int h;
    int kind;
    int index;
};

constexpr Category kCategories[CAT_COUNT] = {
    { "general", "General" },
    { "appearance", "Appearance" },
    { "desktop-dock", "Desktop & Dock" },
    { "mouse", "Mouse & Wheel" },
    { "date-time", "Date & Time" },
    { "display", "Display" },
    { "about", "About" },
};

constexpr const char *kAppearanceChoices[] = { "Light", "Dark", "Auto" };
constexpr const char *kAccentChoices[] = { "Blue", "Graphite", "Forest" };
constexpr const char *kMotionChoices[] = { "Fast", "Reduced" };
constexpr const char *kOnOffChoices[] = { "On", "Off" };
constexpr const char *kDockChoices[] = { "Small", "Medium", "Large" };
constexpr const char *kWallpaperChoices[] = { "Cover", "Center", "Stretch" };
constexpr const char *kMagSpeedChoices[] = { "Fast", "Normal", "Slow" };
constexpr const char *kClockChoices[] = { "24-hour", "12-hour" };
constexpr const char *kTimezoneChoices[] = {
    "UTC-8", "UTC-5", "UTC", "UTC+1", "UTC+3", "UTC+8", "UTC+9"
};

Setting g_settings[] = {
    { "general.appearance", CAT_APPEARANCE, "Appearance", W_DROPDOWN, kAppearanceChoices, 3, 0, 0, 0 },
    { "general.accent", CAT_APPEARANCE, "Accent Color", W_DROPDOWN, kAccentChoices, 3, 0, 0, 0 },
    { "general.motion", CAT_APPEARANCE, "Animation Speed", W_DROPDOWN, kMotionChoices, 2, 0, 0, 0 },

    { "desktop.dock-size", CAT_DESKTOP_DOCK, "Dock Size", W_DROPDOWN, kDockChoices, 3, 0, 0, 1 },
    { "desktop.wallpaper", CAT_DESKTOP_DOCK, "Wallpaper Fit", W_DROPDOWN, kWallpaperChoices, 3, 0, 0, 0 },
    { "dock.magnification", CAT_DESKTOP_DOCK, "Magnification", W_TOGGLE, kOnOffChoices, 2, 0, 0, 0 },
    { "dock.mag-size", CAT_DESKTOP_DOCK, "Magnification Size", W_SLIDER, nullptr, 0, 4, 40, 18 },
    { "dock.mag-range", CAT_DESKTOP_DOCK, "Magnification Range", W_SLIDER, nullptr, 0, 60, 220, 132 },
    { "dock.mag-speed", CAT_DESKTOP_DOCK, "Magnification Speed", W_DROPDOWN, kMagSpeedChoices, 3, 0, 0, 1 },
    { "dock.pin.monitor", CAT_DESKTOP_DOCK, "Pin Activity Monitor", W_TOGGLE, kOnOffChoices, 2, 0, 0, 0 },
    { "dock.pin.terminal", CAT_DESKTOP_DOCK, "Pin Terminal", W_TOGGLE, kOnOffChoices, 2, 0, 0, 0 },
    { "dock.pin.files", CAT_DESKTOP_DOCK, "Pin Files", W_TOGGLE, kOnOffChoices, 2, 0, 0, 0 },
    { "dock.pin.settings", CAT_DESKTOP_DOCK, "Pin System Settings", W_TOGGLE, kOnOffChoices, 2, 0, 0, 0 },

    { "mouse.natural-scroll", CAT_MOUSE, "Natural Scroll", W_TOGGLE, kOnOffChoices, 2, 0, 0, 1 },
    { "mouse.wheel-lines", CAT_MOUSE, "Wheel Lines", W_SLIDER, nullptr, 0, 1, 8, 3 },

    { "datetime.clock", CAT_DATE_TIME, "Clock Format", W_DROPDOWN, kClockChoices, 2, 0, 0, 0 },
    { "datetime.timezone", CAT_DATE_TIME, "Timezone", W_DROPDOWN, kTimezoneChoices, 7, 0, 0, 4 },
};

constexpr int kSettingCount = (int)(sizeof(g_settings) / sizeof(g_settings[0]));

Window g_win;
GxDevice g_gx;
WindowOptions g_win_opts;
ScreenInfo g_screen{};
Input g_prev_input{};
bool g_dirty = true;
int g_clock_poll = 0;
int g_opts_poll = 0;
char g_last_live_clock[32] = "";
int g_active_category = CAT_GENERAL;
int g_hover_sidebar = -1;
int g_hover_setting = -1;
constexpr int kOptsPollEvery = 32;
ClickTarget g_targets[48];
int g_target_count = 0;
bool g_was_minimized = false;

int g_scroll_y = 0;
int g_content_h = 0;
int g_drag_slider = -1; /* setting index while dragging slider */
int g_drag_scroll = 0;
int g_wheel_lines = 3;
bool g_natural_scroll = true;

/* Live input probe (Mouse & Wheel page). */
int32_t g_last_wheel = 0;
uint8_t g_last_buttons = 0;
uint8_t g_last_mods = 0;

void append_text(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0 || !src)
        return;
    size_t len = strlen(dst);
    if (len >= dst_size)
        return;
    strncpy(dst + len, src, dst_size - len - 1);
    dst[dst_size - 1] = 0;
}

void append_uint(char *dst, size_t dst_size, uint32_t value)
{
    char tmp[16];
    int i = 0;
    if (value == 0) {
        append_text(dst, dst_size, "0");
        return;
    }
    while (value > 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (i-- > 0) {
        char ch[2] = { tmp[i], 0 };
        append_text(dst, dst_size, ch);
    }
}

void append_int(char *dst, size_t dst_size, int value)
{
    if (value < 0) {
        append_text(dst, dst_size, "-");
        append_uint(dst, dst_size, (uint32_t)(-value));
    } else {
        append_uint(dst, dst_size, (uint32_t)value);
    }
}

int text_width(const char *s)
{
    return Surface::text_width(s, 1);
}

int clampi(int v, int lo, int hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

void register_target(int y, int h, int kind, int index)
{
    if (g_target_count >= (int)(sizeof(g_targets) / sizeof(g_targets[0])))
        return;
    g_targets[g_target_count].y = y;
    g_targets[g_target_count].h = h;
    g_targets[g_target_count].kind = kind;
    g_targets[g_target_count].index = index;
    g_target_count++;
}

void unlink_deeplink()
{
    (void)hsrc::sdk::syscall1(SYS_UNLINK, (long)kDeepLinkPath);
}

bool refresh_window_options()
{
    if (!g_win.get_options(g_win_opts))
        return false;
    if (g_was_minimized && !g_win_opts.minimized && g_win_opts.visible)
        g_dirty = true;
    g_was_minimized = g_win_opts.minimized;
    return true;
}

int setting_index(const char *key)
{
    for (int i = 0; i < kSettingCount; i++) {
        if (strcmp(g_settings[i].key, key) == 0)
            return i;
    }
    return -1;
}

void sync_mouse_prefs()
{
    int i = setting_index("mouse.wheel-lines");
    if (i >= 0)
        g_wheel_lines = clampi(g_settings[i].current, 1, 8);
    i = setting_index("mouse.natural-scroll");
    if (i >= 0)
        g_natural_scroll = (g_settings[i].current == 0); /* On = index 0 */
}

void format_setting_value(const Setting &s, char *out, size_t out_size)
{
    out[0] = 0;
    if (s.kind == W_SLIDER) {
        append_int(out, out_size, s.current);
        return;
    }
    if (s.choices && s.current >= 0 && s.current < s.choice_count)
        append_text(out, out_size, s.choices[s.current]);
}

void apply_time_settings()
{
    (void)hsrc::sdk::time::init();
    for (int i = 0; i < kSettingCount; i++) {
        if (g_settings[i].kind == W_SLIDER)
            continue;
        const char *value = g_settings[i].choices
                                ? g_settings[i].choices[g_settings[i].current]
                                : "";
        if (strcmp(g_settings[i].key, "datetime.timezone") == 0) {
            int off = 0;
            if (hsrc::sdk::time::parse_timezone_label(value, &off))
                (void)hsrc::sdk::time::set_timezone(off, value);
        } else if (strcmp(g_settings[i].key, "datetime.clock") == 0) {
            (void)hsrc::sdk::time::set_hour12(strcmp(value, "12-hour") == 0);
        }
    }
}

void apply_setting_effects(int idx)
{
    if (idx < 0 || idx >= kSettingCount)
        return;
    const Setting &s = g_settings[idx];

    if (strcmp(s.key, "datetime.timezone") == 0) {
        const char *value = s.choices ? s.choices[s.current] : "";
        int off = 0;
        if (hsrc::sdk::time::parse_timezone_label(value, &off))
            (void)hsrc::sdk::time::set_timezone(off, value);
    } else if (strcmp(s.key, "datetime.clock") == 0) {
        const char *value = s.choices ? s.choices[s.current] : "";
        (void)hsrc::sdk::time::set_hour12(strcmp(value, "12-hour") == 0);
    } else if (strcmp(s.key, "mouse.natural-scroll") == 0 ||
               strcmp(s.key, "mouse.wheel-lines") == 0) {
        sync_mouse_prefs();
    }
}

void persist_setting(int idx)
{
    if (idx < 0 || idx >= kSettingCount)
        return;
    const Setting &s = g_settings[idx];

    if (strcmp(s.key, "general.appearance") == 0) {
        int cur = s.current;
        if (cur < 0)
            cur = 0;
        if (s.choice_count > 0 && cur >= s.choice_count)
            cur = s.choice_count - 1;
        (void)set_appearance(static_cast<Appearance>(cur));
    } else {
        char val[32];
        format_setting_value(s, val, sizeof(val));
        (void)persist_key(s.key, val);
    }
    apply_setting_effects(idx);
}

int parse_int(const char *s, int *ok)
{
    if (!s || !s[0]) {
        if (ok)
            *ok = 0;
        return 0;
    }
    int neg = 0;
    int i = 0;
    if (s[0] == '-') {
        neg = 1;
        i = 1;
    }
    int v = 0;
    int digits = 0;
    while (s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (s[i] - '0');
        i++;
        digits++;
    }
    if (ok)
        *ok = digits > 0;
    return neg ? -v : v;
}

void load_settings()
{
    int fd = (int)hsrc::sdk::open(kIniPath, O_RDONLY);
    if (fd < 0)
        return;

    char buf[kIniBytes];
    memset(buf, 0, sizeof(buf));
    long nread = hsrc::sdk::read(fd, buf, sizeof(buf) - 1);
    (void)hsrc::sdk::close(fd);
    if (nread <= 0)
        return;

    int start = 0;
    for (long i = 0; i <= nread; i++) {
        if (buf[i] != '\n' && buf[i] != '\r' && buf[i] != 0)
            continue;
        buf[i] = 0;
        char *line = buf + start;
        start = (int)i + 1;
        if (!line[0])
            continue;

        char *eq = line;
        while (*eq && *eq != '=')
            eq++;
        if (*eq != '=')
            continue;
        *eq++ = 0;

        for (int s = 0; s < kSettingCount; s++) {
            if (strcmp(line, g_settings[s].key) != 0)
                continue;
            if (g_settings[s].kind == W_SLIDER) {
                int ok = 0;
                int v = parse_int(eq, &ok);
                if (ok)
                    g_settings[s].current = clampi(v, g_settings[s].min_v, g_settings[s].max_v);
            } else {
                for (int c = 0; c < g_settings[s].choice_count; c++) {
                    if (strcmp(eq, g_settings[s].choices[c]) == 0) {
                        g_settings[s].current = c;
                        break;
                    }
                }
            }
            break;
        }
    }
    sync_mouse_prefs();
}

int category_from_id(const char *id)
{
    if (!id || !id[0])
        return CAT_GENERAL;
    if (strcmp(id, "system-information") == 0 || strcmp(id, "about") == 0)
        return CAT_ABOUT;
    if (strcmp(id, "input") == 0 || strcmp(id, "keyboard") == 0)
        return CAT_MOUSE;
    for (int i = 0; i < CAT_COUNT; i++) {
        if (strcmp(id, kCategories[i].id) == 0)
            return i;
    }
    return CAT_GENERAL;
}

bool poll_deeplink()
{
    int fd = (int)hsrc::sdk::open(kDeepLinkPath, O_RDONLY);
    if (fd < 0)
        return false;

    char buf[kDeepLinkBytes];
    memset(buf, 0, sizeof(buf));
    long nread = hsrc::sdk::read(fd, buf, sizeof(buf) - 1);
    (void)hsrc::sdk::close(fd);
    unlink_deeplink();

    if (nread <= 0 || buf[0] == 0)
        return false;

    const char *id = buf;
    if (strncmp(buf, "settings://", 11) == 0)
        id = buf + 11;
    while (*id == '/' || *id == ' ')
        id++;

    char clean[64];
    size_t n = 0;
    while (id[n] && id[n] != '\n' && id[n] != '\r' && id[n] != ' ' && n + 1 < sizeof(clean)) {
        clean[n] = id[n];
        n++;
    }
    clean[n] = 0;

    g_active_category = category_from_id(clean);
    g_hover_setting = -1;
    g_scroll_y = 0;
    g_dirty = true;
    return true;
}

int max_scroll()
{
    int m = g_content_h - kViewH;
    return m > 0 ? m : 0;
}

void clamp_scroll()
{
    g_scroll_y = clampi(g_scroll_y, 0, max_scroll());
}

void apply_wheel(int32_t wheel)
{
    if (wheel == 0)
        return;
    g_last_wheel = wheel;
    int32_t delta = wheel;
    if (g_natural_scroll)
        delta = -delta;
    /* PS/2: +wheel often means away from user; natural maps like macOS. */
    g_scroll_y -= (int)delta * kRowH * g_wheel_lines;
    clamp_scroll();
    g_dirty = true;
}

int sidebar_hit(int lx, int ly)
{
    const int top = ui_panel_body_top(2);
    if (lx < 8 || lx >= kSidebarW - 8 || ly < top)
        return -1;
    for (int i = 0; i < CAT_COUNT; i++) {
        int y = top + i * kSidebarRowStep;
        if (ly >= y && ly < y + kSidebarRowH)
            return i;
    }
    return -1;
}

const ClickTarget *target_hit(int lx, int ly)
{
    int content_ly = ly + g_scroll_y;
    for (int i = 0; i < g_target_count; i++) {
        if (g_targets[i].kind == TARGET_SCROLLBAR) {
            if (lx >= kScrollX - 2 && lx < kScrollX + kScrollW + 2 &&
                ly >= kContentTop && ly < kContentBot)
                return &g_targets[i];
            continue;
        }
        if (lx < kContentX || lx >= kContentX + kContentW)
            continue;
        if (ly < kContentTop || ly >= kContentBot)
            continue;
        if (content_ly >= g_targets[i].y && content_ly < g_targets[i].y + g_targets[i].h)
            return &g_targets[i];
    }
    return nullptr;
}

void cycle_setting(int idx)
{
    if (idx < 0 || idx >= kSettingCount)
        return;
    Setting &s = g_settings[idx];
    if (s.kind == W_SLIDER)
        return;
    s.current++;
    if (s.current >= s.choice_count)
        s.current = 0;
    persist_setting(idx);
    g_dirty = true;
}

void toggle_setting(int idx)
{
    if (idx < 0 || idx >= kSettingCount)
        return;
    Setting &s = g_settings[idx];
    if (s.kind != W_TOGGLE)
        return;
    s.current = (s.current == 0) ? 1 : 0;
    persist_setting(idx);
    g_dirty = true;
}

void set_slider_from_x(int idx, int lx)
{
    if (idx < 0 || idx >= kSettingCount)
        return;
    Setting &s = g_settings[idx];
    if (s.kind != W_SLIDER)
        return;
    const int track_x = kContentX + kContentW - 160;
    const int track_w = 140;
    int t = lx - track_x;
    t = clampi(t, 0, track_w);
    int span = s.max_v - s.min_v;
    if (span <= 0)
        span = 1;
    int v = s.min_v + (t * span) / track_w;
    v = clampi(v, s.min_v, s.max_v);
    if (v != s.current) {
        s.current = v;
        persist_setting(idx);
        g_dirty = true;
    }
}

void paint_sidebar_row(Surface &s, int i)
{
    if (i < 0 || i >= CAT_COUNT)
        return;
    const auto &t = theme();
    const int top = ui_panel_body_top(2);
    const int y = top + i * kSidebarRowStep;
    const int text_dy = ui_text_inset_y(kSidebarRowH);
    const bool selected = (i == g_active_category);
    const bool hover = (i == g_hover_sidebar);

    s.fill(8, y, kSidebarW - 16, kSidebarRowH, t.sidebar);
    if (selected)
        s.fill(8, y, kSidebarW - 16, kSidebarRowH, t.accent_soft);
    else if (hover)
        s.fill(8, y, kSidebarW - 16, kSidebarRowH, t.hover);
    s.text(16, y + text_dy, kCategories[i].label, selected ? t.accent : t.text, 1);
}

void draw_sidebar(Surface &s)
{
    const auto &t = theme();
    s.fill(0, kChromeTitleH, kSidebarW, kWinH - kChromeTitleH, t.sidebar);
    s.fill(kSidebarW - 1, kChromeTitleH, 1, kWinH - kChromeTitleH, t.border);
    s.text(kPad, ui_panel_text_y(0), "Settings", t.text, 1);
    s.text(kPad, ui_panel_text_y(1), "preferences", t.text_dim, 1);

    for (int i = 0; i < CAT_COUNT; i++)
        paint_sidebar_row(s, i);
}

void draw_scrollbar(Surface &s)
{
    const auto &t = theme();
    int max_s = max_scroll();
    if (max_s <= 0)
        return;

    s.fill_round(kScrollX, kContentTop, kScrollW, kViewH, 4, t.inset);
    int thumb_h = (kViewH * kViewH) / g_content_h;
    if (thumb_h < 24)
        thumb_h = 24;
    if (thumb_h > kViewH)
        thumb_h = kViewH;
    int thumb_y = kContentTop + (g_scroll_y * (kViewH - thumb_h)) / max_s;
    s.fill_round(kScrollX, thumb_y, kScrollW, thumb_h, 4, t.accent);
    register_target(0, 0, TARGET_SCROLLBAR, 0);
}

void draw_dropdown_row(Surface &s, int y, int idx, bool hover, bool track = true)
{
    const auto &t = theme();
    const Setting &st = g_settings[idx];
    char val[40];
    format_setting_value(st, val, sizeof(val));
    const int text_dy = ui_text_inset_y(kRowH);

    s.fill(kContentX, y, kContentW, kRowH, t.bg);
    if (hover)
        s.fill(kContentX, y, kContentW, kRowH, t.hover);
    s.text(kContentX + 4, y + text_dy, st.label, t.text, 1);

    const int bx = kContentX + kContentW - 150;
    s.fill_round(bx, y + 6, 140, kRowH - 12, 6, t.button);
    s.rect(bx, y + 6, 140, kRowH - 12, hover ? t.accent : t.border, 1);
    s.text(bx + 10, y + text_dy, val, t.accent, 1);
    s.text(bx + 120, y + text_dy, "v", t.text_dim, 1);
    s.fill(kContentX, y + kRowH - 1, kContentW, 1, t.border);
    if (track)
        register_target(y + g_scroll_y, kRowH, TARGET_SETTING, idx);
}

void draw_toggle_row(Surface &s, int y, int idx, bool hover, bool track = true)
{
    const auto &t = theme();
    const Setting &st = g_settings[idx];
    bool on = (st.current == 0);
    const int text_dy = ui_text_inset_y(kRowH);

    s.fill(kContentX, y, kContentW, kRowH, t.bg);
    if (hover)
        s.fill(kContentX, y, kContentW, kRowH, t.hover);
    s.text(kContentX + 4, y + text_dy, st.label, t.text, 1);

    const int bx = kContentX + kContentW - 56;
    s.fill_round(bx, y + 10, 44, 16, 8, on ? t.accent : t.inset);
    int knob_x = on ? bx + 26 : bx + 2;
    s.fill_round(knob_x, y + 12, 12, 12, 6, t.panel);
    s.fill(kContentX, y + kRowH - 1, kContentW, 1, t.border);
    if (track)
        register_target(y + g_scroll_y, kRowH, TARGET_SETTING, idx);
}

void draw_slider_row(Surface &s, int y, int idx, bool hover, bool track = true)
{
    const auto &t = theme();
    const Setting &st = g_settings[idx];
    char val[16];
    format_setting_value(st, val, sizeof(val));
    const int text_dy = ui_text_inset_y(kRowH);

    s.fill(kContentX, y, kContentW, kRowH, t.bg);
    if (hover)
        s.fill(kContentX, y, kContentW, kRowH, t.hover);
    s.text(kContentX + 4, y + text_dy, st.label, t.text, 1);
    s.text(kContentX + kContentW - 200, y + text_dy, val, t.accent, 1);

    const int track_x = kContentX + kContentW - 160;
    const int track_w = 140;
    const int track_y = y + kRowH / 2 - 2;
    s.fill_round(track_x, track_y, track_w, 4, 2, t.inset);
    int span = st.max_v - st.min_v;
    if (span <= 0)
        span = 1;
    int thumb = ((st.current - st.min_v) * track_w) / span;
    s.fill_round(track_x, track_y, thumb, 4, 2, t.accent);
    s.fill_round(track_x + thumb - 5, track_y - 4, 10, 12, 5, t.accent);
    s.fill(kContentX, y + kRowH - 1, kContentW, 1, t.border);
    if (track)
        register_target(y + g_scroll_y, kRowH, TARGET_SETTING, idx);
}

void begin_page(Surface &s, const char *title, const char *hint)
{
    const auto &t = theme();
    g_target_count = 0;
    s.text(kContentX, ui_panel_text_y(0), title, t.text, 1);
    s.text(kContentX, ui_panel_text_y(1), hint, t.text_dim, 1);
}

void draw_info_row(Surface &s, int y, const char *label, const char *value)
{
    const auto &t = theme();
    const int text_dy = ui_text_inset_y(kRowH);
    s.text(kContentX + 4, y + text_dy, label, t.text, 1);
    int value_x = kContentX + kContentW - 4 - text_width(value);
    if (value_x < kContentX + 200)
        value_x = kContentX + 200;
    s.text(value_x, y + text_dy, value, t.text_dim, 1);
    s.fill(kContentX, y + kRowH - 1, kContentW, 1, t.border);
}

void paint()
{
    if (!g_win.ok())
        return;

    const auto &t = theme();
    Surface &s = g_win.surface();
    s.fill(0, kChromeTitleH, kWinW, kWinH - kChromeTitleH, t.bg);
    draw_sidebar(s);

    /* Clip content band by filling over later? Simple approach: draw rows with scroll offset. */
    int y = kContentTop - g_scroll_y;
    int y_start = y;

    auto visible = [&](int row_y) -> bool {
        return row_y + kRowH > kContentTop && row_y < kContentBot;
    };

    g_target_count = 0;

    switch (g_active_category) {
    case CAT_GENERAL:
        begin_page(s, "General", "Shell preferences and tips.");
        y = kContentTop - g_scroll_y;
        if (visible(y))
            draw_info_row(s, y, "Ini path", "/etc/os-settings.ini");
        y += kRowH + kRowGap;
        if (visible(y))
            draw_info_row(s, y, "Dock layout", "pinned | live unpinned");
        y += kRowH + kRowGap;
        if (visible(y))
            draw_info_row(s, y, "Tip", "Desktop & Dock → magnification sliders");
        y += kRowH + kRowGap;
        break;

    case CAT_APPEARANCE:
        begin_page(s, "Appearance", "Dropdowns cycle on click.");
        y = kContentTop - g_scroll_y;
        for (int i = 0; i < kSettingCount; i++) {
            if (g_settings[i].category != CAT_APPEARANCE)
                continue;
            if (visible(y)) {
                if (g_settings[i].kind == W_DROPDOWN)
                    draw_dropdown_row(s, y, i, i == g_hover_setting);
                else if (g_settings[i].kind == W_TOGGLE)
                    draw_toggle_row(s, y, i, i == g_hover_setting);
                else
                    draw_slider_row(s, y, i, i == g_hover_setting);
            } else {
                /* Still register for hit-testing in scrolled space */
                register_target(y + g_scroll_y, kRowH, TARGET_SETTING, i);
            }
            y += kRowH + kRowGap;
        }
        break;

    case CAT_DESKTOP_DOCK:
        begin_page(s, "Desktop & Dock", "Magnification size/range + pins. Scroll with wheel.");
        y = kContentTop - g_scroll_y;
        for (int i = 0; i < kSettingCount; i++) {
            if (g_settings[i].category != CAT_DESKTOP_DOCK)
                continue;
            if (visible(y)) {
                if (g_settings[i].kind == W_SLIDER)
                    draw_slider_row(s, y, i, i == g_hover_setting);
                else if (g_settings[i].kind == W_TOGGLE)
                    draw_toggle_row(s, y, i, i == g_hover_setting);
                else
                    draw_dropdown_row(s, y, i, i == g_hover_setting);
            } else {
                register_target(y + g_scroll_y, kRowH, TARGET_SETTING, i);
            }
            y += kRowH + kRowGap;
        }
        break;

    case CAT_MOUSE: {
        begin_page(s, "Mouse & Wheel", "Native IntelliMouse wheel + side buttons + Super/Menu.");
        y = kContentTop - g_scroll_y;

        char wbuf[32];
        char bbuf[48];
        char mbuf[48];
        wbuf[0] = 0;
        append_int(wbuf, sizeof(wbuf), (int)g_last_wheel);
        bbuf[0] = 0;
        if (g_last_buttons & UGX_BTN_LEFT)
            append_text(bbuf, sizeof(bbuf), "L ");
        if (g_last_buttons & UGX_BTN_RIGHT)
            append_text(bbuf, sizeof(bbuf), "R ");
        if (g_last_buttons & UGX_BTN_MIDDLE)
            append_text(bbuf, sizeof(bbuf), "M ");
        if (g_last_buttons & UGX_BTN_BACK)
            append_text(bbuf, sizeof(bbuf), "Back ");
        if (g_last_buttons & UGX_BTN_FORWARD)
            append_text(bbuf, sizeof(bbuf), "Fwd ");
        if (!bbuf[0])
            append_text(bbuf, sizeof(bbuf), "(none)");
        mbuf[0] = 0;
        if (g_last_mods & KBD_MOD_SHIFT)
            append_text(mbuf, sizeof(mbuf), "Shift ");
        if (g_last_mods & KBD_MOD_CTRL)
            append_text(mbuf, sizeof(mbuf), "Ctrl ");
        if (g_last_mods & KBD_MOD_ALT)
            append_text(mbuf, sizeof(mbuf), "Alt ");
        if (g_last_mods & KBD_MOD_ALTGR)
            append_text(mbuf, sizeof(mbuf), "AltGr ");
        if (g_last_mods & KBD_MOD_SUPER)
            append_text(mbuf, sizeof(mbuf), "Super ");
        if (g_last_mods & KBD_MOD_MENU)
            append_text(mbuf, sizeof(mbuf), "Menu ");
        if (!mbuf[0])
            append_text(mbuf, sizeof(mbuf), "(none)");

        if (visible(y))
            draw_info_row(s, y, "Last wheel delta", wbuf);
        y += kRowH + kRowGap;
        if (visible(y))
            draw_info_row(s, y, "Buttons", bbuf);
        y += kRowH + kRowGap;
        if (visible(y))
            draw_info_row(s, y, "Modifiers", mbuf);
        y += kRowH + kRowGap;

        for (int i = 0; i < kSettingCount; i++) {
            if (g_settings[i].category != CAT_MOUSE)
                continue;
            if (visible(y)) {
                if (g_settings[i].kind == W_SLIDER)
                    draw_slider_row(s, y, i, i == g_hover_setting);
                else if (g_settings[i].kind == W_TOGGLE)
                    draw_toggle_row(s, y, i, i == g_hover_setting);
                else
                    draw_dropdown_row(s, y, i, i == g_hover_setting);
            } else {
                register_target(y + g_scroll_y, kRowH, TARGET_SETTING, i);
            }
            y += kRowH + kRowGap;
        }
        break;
    }

    case CAT_DATE_TIME: {
        begin_page(s, "Date & Time", "Timezone and clock format drive the menubar clock.");
        y = kContentTop - g_scroll_y;
        char live[48];
        char iso[40];
        hsrc::sdk::time::format_clock(live, sizeof(live));
        hsrc::sdk::time::format_iso_local(iso, sizeof(iso));
        if (visible(y))
            draw_info_row(s, y, "Now (local)", live);
        y += kRowH + kRowGap;
        if (visible(y))
            draw_info_row(s, y, "ISO local", iso);
        y += kRowH + kRowGap;
        for (int i = 0; i < kSettingCount; i++) {
            if (g_settings[i].category != CAT_DATE_TIME)
                continue;
            if (visible(y))
                draw_dropdown_row(s, y, i, i == g_hover_setting);
            else
                register_target(y + g_scroll_y, kRowH, TARGET_SETTING, i);
            y += kRowH + kRowGap;
        }
        break;
    }

    case CAT_DISPLAY: {
        begin_page(s, "Display", "Live screen_info() readout.");
        y = kContentTop - g_scroll_y;
        char resolution[40];
        char depth[24];
        resolution[0] = 0;
        append_uint(resolution, sizeof(resolution), g_screen.width);
        append_text(resolution, sizeof(resolution), " x ");
        append_uint(resolution, sizeof(resolution), g_screen.height);
        depth[0] = 0;
        append_uint(depth, sizeof(depth), g_screen.bpp);
        append_text(depth, sizeof(depth), "-bit");
        if (visible(y))
            draw_info_row(s, y, "Resolution", resolution);
        y += kRowH + kRowGap;
        if (visible(y))
            draw_info_row(s, y, "Color Depth", depth);
        y += kRowH + kRowGap;
        if (visible(y))
            draw_info_row(s, y, "Window Server", "MKDX");
        y += kRowH + kRowGap;
        break;
    }

    case CAT_ABOUT:
    default: {
        begin_page(s, "About", "HSRC OS control center.");
        y = kContentTop - g_scroll_y;
        if (visible(y))
            draw_info_row(s, y, "Product", "HSRC OS");
        y += kRowH + kRowGap;
        if (visible(y))
            draw_info_row(s, y, "Settings", "os-settings.mke");
        y += kRowH + kRowGap;
        if (visible(y))
            draw_info_row(s, y, "Dock", "pinned | running unpinned");
        y += kRowH + kRowGap;
        if (visible(y))
            draw_info_row(s, y, "Input", "wheel · side · Super/Menu");
        y += kRowH + kRowGap;
        break;
    }
    }

    g_content_h = (y + g_scroll_y) - kContentTop + 8;
    if (g_content_h < kViewH)
        g_content_h = kViewH;
    clamp_scroll();

    /* Cover chrome/header over scrolled overflow */
    s.fill(kSidebarW, kChromeTitleH, kWinW - kSidebarW, kContentTop - kChromeTitleH, t.bg);
    s.text(kContentX, ui_panel_text_y(0), kCategories[g_active_category].label, t.text, 1);

    draw_scrollbar(s);

    (void)y_start;
    g_dirty = false;
}

int setting_row_screen_y(int idx)
{
    for (int i = 0; i < g_target_count; i++) {
        if (g_targets[i].kind == TARGET_SETTING && g_targets[i].index == idx)
            return g_targets[i].y - g_scroll_y;
    }
    return -10000;
}

void paint_setting_row(Surface &s, int idx)
{
    if (idx < 0 || idx >= kSettingCount)
        return;
    int y = setting_row_screen_y(idx);
    if (y + kRowH <= kContentTop || y >= kContentBot)
        return;
    bool hover = (idx == g_hover_setting);
    if (g_settings[idx].kind == W_DROPDOWN)
        draw_dropdown_row(s, y, idx, hover, false);
    else if (g_settings[idx].kind == W_TOGGLE)
        draw_toggle_row(s, y, idx, hover, false);
    else
        draw_slider_row(s, y, idx, hover, false);
    g_win.damage(kContentX, y, kContentW, kRowH);
}

void paint_hover_delta(int old_sidebar, int new_sidebar, int old_setting, int new_setting)
{
    if (!g_win.ok())
        return;
    Surface &s = g_win.surface();
    const int top = ui_panel_body_top(2);

    if (old_sidebar != new_sidebar) {
        if (old_sidebar >= 0) {
            paint_sidebar_row(s, old_sidebar);
            g_win.damage(8, top + old_sidebar * kSidebarRowStep,
                         kSidebarW - 16, kSidebarRowH);
        }
        if (new_sidebar >= 0) {
            paint_sidebar_row(s, new_sidebar);
            g_win.damage(8, top + new_sidebar * kSidebarRowStep,
                         kSidebarW - 16, kSidebarRowH);
        }
    }
    if (old_setting != new_setting) {
        if (old_setting >= 0)
            paint_setting_row(s, old_setting);
        if (new_setting >= 0)
            paint_setting_row(s, new_setting);
    }
}

void update_hover(int lx, int ly)
{
    int next_sidebar = sidebar_hit(lx, ly);
    int next_setting = -1;
    const ClickTarget *hit = target_hit(lx, ly);
    if (hit && hit->kind == TARGET_SETTING)
        next_setting = hit->index;

    if (next_sidebar == g_hover_sidebar && next_setting == g_hover_setting)
        return;

    int old_sidebar = g_hover_sidebar;
    int old_setting = g_hover_setting;
    g_hover_sidebar = next_sidebar;
    g_hover_setting = next_setting;
    /* Hover only: local row redraw — never full-window clear/paint. */
    paint_hover_delta(old_sidebar, next_sidebar, old_setting, next_setting);
}

void handle_click(const Input &in)
{
    if (!refresh_window_options())
        return;
    if (g_win_opts.minimized || !g_win_opts.visible)
        return;

    int lx = in.mouse_x - g_win_opts.x;
    int ly = in.mouse_y - g_win_opts.y;
    if (lx < 0 || ly < 0 || lx >= g_win_opts.w || ly >= g_win_opts.h)
        return;

    const ClickTarget *hit = target_hit(lx, ly);
    if (hit && hit->kind == TARGET_SCROLLBAR) {
        g_drag_scroll = 1;
        int max_s = max_scroll();
        if (max_s > 0) {
            int rel = ly - kContentTop;
            g_scroll_y = (rel * max_s) / kViewH;
            clamp_scroll();
            g_dirty = true;
        }
        return;
    }

    if (hit && hit->kind == TARGET_SETTING) {
        int idx = hit->index;
        if (idx >= 0 && idx < kSettingCount) {
            if (g_settings[idx].kind == W_SLIDER) {
                g_drag_slider = idx;
                set_slider_from_x(idx, lx);
                return;
            }
            if (g_settings[idx].kind == W_TOGGLE) {
                toggle_setting(idx);
                return;
            }
            cycle_setting(idx);
            return;
        }
    }

    int cat = sidebar_hit(lx, ly);
    if (cat >= 0 && g_active_category != cat) {
        g_active_category = cat;
        g_hover_setting = -1;
        g_scroll_y = 0;
        g_dirty = true;
    }
}

} // namespace

extern "C" void mke_main(void)
{
    g_dirty = true;

    if (!hsrc::sdk::screen_info(g_screen) || g_screen.width == 0 || g_screen.height == 0) {
        for (;;)
            hsrc::sdk::this_thread::sleep_for(1000u);
    }

    load_settings();
    (void)refresh_theme();
    apply_time_settings();
    sync_mouse_prefs();

    WindowOptions opts;
    opts.x = (int)g_screen.width > kWinW ? ((int)g_screen.width - kWinW) / 2 : 40;
    opts.y = (int)g_screen.height > kWinH ? ((int)g_screen.height - kWinH) / 2 : 40;
    opts.w = kWinW;
    opts.h = kWinH;
    opts.radius = 10;
    opts.rounded = true;
    opts.shadow = true;
    opts.resizable = false;
    opts.framed = true;
    opts.closable = true;
    opts.can_minimize = true;
    opts.can_maximize = true;
    opts.accept_focus = true;
    opts.set_title("System Settings");
    opts.set_class_name("os.settings");

    if (!g_win.create(opts))
        hsrc::sdk::exit(1);
    if (!g_gx.create(g_win))
        hsrc::sdk::exit(1);
    (void)refresh_window_options();

    g_win.show(true);
    g_win.focus();
    if (poll_deeplink())
        (void)refresh_window_options();
    g_gx.set_chrome_colors(theme().chrome, theme().text, theme().border);

    for (;;) {
        if (!g_win.ok()) {
            (void)g_win.close();
            hsrc::sdk::exit(0);
        }

        const bool deeplink_nav = poll_deeplink();
        if (deeplink_nav)
            (void)refresh_window_options();

        if (refresh_theme()) {
            g_gx.set_chrome_colors(theme().chrome, theme().text, theme().border);
            g_dirty = true;
        }

        if (g_active_category == CAT_DATE_TIME) {
            if (++g_clock_poll >= 8) {
                g_clock_poll = 0;
                char live[32];
                hsrc::sdk::time::format_clock(live, sizeof(live));
                if (strcmp(live, g_last_live_clock) != 0) {
                    strncpy(g_last_live_clock, live, sizeof(g_last_live_clock) - 1);
                    g_last_live_clock[sizeof(g_last_live_clock) - 1] = 0;
                    g_dirty = true;
                }
            }
        }

        g_opts_poll++;
        if (g_opts_poll >= kOptsPollEvery) {
            g_opts_poll = 0;
            (void)refresh_window_options();
        }

        uint32_t wait_to = kThemeWaitTicks;
        if (g_win_opts.minimized)
            wait_to = 200u;
        else if (g_drag_slider >= 0 || g_drag_scroll)
            wait_to = 1u;
        else if (g_active_category == CAT_DATE_TIME)
            wait_to = 8u;

        Input in = g_gx.wait(wait_to);

        {
            const uint8_t btn_delta = (uint8_t)(in.buttons ^ g_prev_input.buttons);
            if (btn_delta || in.wheel != 0) {
                g_opts_poll = kOptsPollEvery;
                (void)refresh_window_options();
            }

            const bool interactive = !g_win_opts.minimized && g_win_opts.visible;

            if (in.wheel != 0 && interactive && in.focus_id == g_win.id())
                apply_wheel(in.wheel);

            if (in.buttons != g_last_buttons || in.mods != g_last_mods || in.wheel != 0) {
                g_last_buttons = in.buttons;
                g_last_mods = in.mods;
                if (g_active_category == CAT_MOUSE)
                    g_dirty = true;
            }

            if (interactive && (in.hit_id == g_win.id() || g_drag_slider >= 0 || g_drag_scroll)) {
                int lx = in.mouse_x - g_win_opts.x;
                int ly = in.mouse_y - g_win_opts.y;
                if (in.hit_id == g_win.id() &&
                    lx >= 0 && ly >= 0 && lx < g_win_opts.w && ly < g_win_opts.h)
                    update_hover(lx, ly);

                if (g_drag_slider >= 0 && (in.buttons & UGX_BTN_LEFT))
                    set_slider_from_x(g_drag_slider, lx);
                if (g_drag_scroll && (in.buttons & UGX_BTN_LEFT)) {
                    int max_s = max_scroll();
                    if (max_s > 0) {
                        int rel = ly - kContentTop;
                        g_scroll_y = clampi((rel * max_s) / kViewH, 0, max_s);
                        g_dirty = true;
                    }
                }
            }

            uint8_t pressed = (uint8_t)(in.buttons & ~g_prev_input.buttons);
            uint8_t released = (uint8_t)(g_prev_input.buttons & ~in.buttons);

            if (pressed & UGX_BTN_LEFT) {
                if (interactive && in.hit_id == g_win.id())
                    handle_click(in);
            }
            if (released & UGX_BTN_LEFT) {
                if (g_drag_slider >= 0)
                    persist_setting(g_drag_slider);
                g_drag_slider = -1;
                g_drag_scroll = 0;
            }

            if (pressed & UGX_BTN_BACK) {
                if (g_active_category > 0) {
                    g_active_category--;
                    g_scroll_y = 0;
                    g_dirty = true;
                }
            }
            if (pressed & UGX_BTN_FORWARD) {
                if (g_active_category + 1 < CAT_COUNT) {
                    g_active_category++;
                    g_scroll_y = 0;
                    g_dirty = true;
                }
            }

            g_prev_input = in;
        }

        if (deeplink_nav)
            g_dirty = true;

        if ((!g_win_opts.minimized && g_dirty) || deeplink_nav) {
            (void)g_gx.begin_scene();
            paint();
            (void)g_gx.end_scene();
            (void)g_gx.present();
        }
    }
}
