#include <user/mke.h>
#include <user/sdk/gfx.hpp>
#include <user/sdk/process.hpp>
#include <user/sdk/settings.hpp>
#include <user/sdk/svg.hpp>
#include <user/sdk/time.hpp>
#include <user/sdk/syscall.hpp>
#include <user/sdk/sync.hpp>
#include <user/sdk/thread.hpp>
#include <user/sdk/fs.hpp>
#include <user/string.h>

/*
 * HSRC OS — desktop shell: menubar + dynamic macOS-style dock.
 *
 * Dock layout:  [ pinned apps ]  |  [ running unpinned apps ]
 * Pin state comes from /etc/os-settings.ini (dock.pin.*).
 * Click: launch / focus / unminimize / minimize (Apple-like toggle).
 */

namespace {

using hsrc::sdk::Color;
using hsrc::sdk::GxDevice;
using hsrc::sdk::Input;
using hsrc::sdk::ScreenInfo;
using hsrc::sdk::Surface;
using hsrc::sdk::SvgIcon;
using hsrc::sdk::Window;
using hsrc::sdk::WindowOptions;
using hsrc::sdk::kTransparent;
using hsrc::sdk::rgb;
using hsrc::sdk::rgba;
using hsrc::sdk::settings::ThemeMode;
using hsrc::sdk::settings::theme_mode;
using hsrc::sdk::settings::toggle_theme;
using hsrc::sdk::settings::refresh_theme;
using hsrc::sdk::settings::refresh_status;
using hsrc::sdk::settings::status;

constexpr int kMenubarH = 28;
constexpr int kDockTrayH = 72;   /* acrylic tray body */
constexpr int kDockMagPad = 48;  /* headroom above tray for magnification */
constexpr int kDockH = kDockMagPad + kDockTrayH;
constexpr int kDockPad = 18;
constexpr int kDockGap = 14;
constexpr int kDockRad = 20;
constexpr int kMagFP = 16;       /* fixed-point for mag lerp (px * kMagFP) */
constexpr int kSepW = 10;
constexpr int kDefaultMagSize = 18;
constexpr int kDefaultMagRange = 132;
constexpr int kDefaultMagLerp = 3;
constexpr int kMaxSlots = 10;
constexpr int kScanEvery = 12;
constexpr int kPinPollEvery = 64;
constexpr int kBounceFrames = 18;
constexpr int kIniBytes = 2048;
constexpr int kStatusIcon = 16;
constexpr int kStatusGap = 12;
constexpr int kStatusRightPad = 10;
constexpr int kThemePollEvery = 96;
constexpr int kStatusPollEvery = 160;

constexpr Color kDesktop = rgb(48, 92, 140);
constexpr Color kMenubarAccent = rgb(35, 131, 226);
constexpr Color kDockFg = rgb(255, 255, 255);
constexpr Color kWatermark = rgba(255, 255, 255, 48);
constexpr Color kDotLive = rgb(90, 200, 120);
constexpr Color kDotHidden = rgba(255, 255, 255, 110);

enum AppId {
    APP_MONITOR = 0,
    APP_TERMINAL,
    APP_FILES,
    APP_SETTINGS,
    APP_IMGUI,
    APP_COUNT
};

struct AppDef {
    const char *id;
    const char *label;
    const char *title;
    const char *class_name;
    const char *path;
    const char *pin_key;
    Color       color;
    bool        default_pinned;
};

constexpr AppDef kApps[APP_COUNT] = {
    { "monitor",  "Mon",  "Activity Monitor", "os.activity-monitor",
      "/applications/activity-monitor.mke", "dock.pin.monitor",  rgb(75, 180, 120),  true },
    { "terminal", "Term", "Terminal",         "os.terminal",
      "/applications/terminal.mke",         "dock.pin.terminal", rgb(36, 36, 40),    true },
    { "files",    "Files","Files",            "os.files",
      "/applications/files.mke",            "dock.pin.files",    rgb(255, 190, 60),  true },
    { "settings", "Prefs","System Settings",  "os.settings",
      "/applications/os-settings.mke",      "dock.pin.settings", rgb(150, 150, 160), true },
    { "imgui",    "ImGui","ImGui Demo",       "imgui.demo",
      "/applications/imgui-demo.mke",       "dock.pin.imgui",    rgb(90, 140, 220),  true },
};

struct DockSlot {
    int  app = -1;
    bool pinned = false;
    bool running = false;
    bool minimized = false;
    bool focused = false;
    int  window_id = -1;
    int  bounce = 0;
    int  appear = 0; /* 0..12 intro scale-in */
    int  mag = 0;        /* animated boost, fixed-point (px * kMagFP) */
    int  mag_target = 0; /* hover target, same units */
};

Window g_desktop;
Window g_menubar;
Window g_dock;
GxDevice g_gx;

int g_sw = 0;
int g_sh = 0;
int g_dock_x = 0;
int g_dock_y = 0;
int g_dock_w = 0;       /* fixed max surface width */
int g_tray_w = 0;       /* painted / hit tray width */
int g_tray_target = 0;
int g_icon = 52;
int g_hover = -1;          /* nearest slot under cursor (click target) */
int g_mag_cursor_x = -1;   /* dock-local X while pointer is in tray; else -1 */
bool g_mag_enabled = true;
int g_mag_size = kDefaultMagSize;   /* max boost px from ini */
int g_mag_range = kDefaultMagRange; /* influence radius px */
int g_mag_lerp = kDefaultMagLerp;   /* lerp divisor Fast=2 Normal=3 Slow=5 */
int g_menu_hover = -1;
int g_status_hover = -1; /* 0=theme 1=wifi 2=battery 3=clock */
int g_focus_id = -1;
uint8_t g_prev_buttons = 0;
int g_scan_tick = 0;
int g_pin_tick = 0;
int g_theme_poll = 0;
int g_status_poll = 0;
char g_clock_text[24] = "";

struct StatusSlot {
    int x, y, w, h;
};
StatusSlot g_slot_theme{};
StatusSlot g_slot_wifi{};
StatusSlot g_slot_battery{};
StatusSlot g_slot_clock{};

SvgIcon g_icon_sun;
SvgIcon g_icon_moon;
SvgIcon g_icon_wifi;
SvgIcon g_icon_wifi_off;
SvgIcon g_icon_battery;
SvgIcon g_icon_bolt;
bool g_pinned[APP_COUNT];
bool g_running[APP_COUNT];
bool g_minimized[APP_COUNT];
int  g_win_id[APP_COUNT];
/* Blocks rapid re-spawn while the first instance is still mapping its window. */
int  g_spawn_cool[APP_COUNT];
/* Extra backoff after a spawn that never produced a window (OOM / crash). */
int  g_spawn_fail[APP_COUNT];
bool g_spawn_pending[APP_COUNT];
DockSlot g_slots[kMaxSlots];
int g_slot_count = 0;
int g_sep_after = -1; /* index after which separator is drawn; -1 = none */
constexpr int kSpawnCoolFrames = 90;
constexpr int kSpawnFailFrames = 240;

void note_spawn_attempt(int app)
{
    if (app < 0 || app >= APP_COUNT)
        return;
    g_spawn_cool[app] = kSpawnCoolFrames;
    g_spawn_pending[app] = true;
}

constexpr int kMenuSettingsX = 92;
constexpr int kMenuSystemInfoX = 160;

int text_width(const char *s)
{
    return Surface::text_width(s, 1);
}

int slot_local_x(int index);

Color menubar_fg()
{
    return theme_mode() == ThemeMode::Dark ? rgb(235, 235, 240) : rgb(28, 28, 30);
}

void format_percent(char *out, size_t out_sz, int percent)
{
    if (!out || out_sz < 3)
        return;
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;
    char tmp[4];
    int n = 0;
    if (percent >= 100) {
        tmp[n++] = '1';
        tmp[n++] = '0';
        tmp[n++] = '0';
    } else if (percent >= 10) {
        tmp[n++] = (char)('0' + percent / 10);
        tmp[n++] = (char)('0' + percent % 10);
    } else {
        tmp[n++] = (char)('0' + percent);
    }
    size_t i = 0;
    for (int k = 0; k < n && i + 1 < out_sz; k++)
        out[i++] = tmp[k];
    if (i + 1 < out_sz)
        out[i++] = '%';
    out[i] = 0;
}

bool load_status_icons()
{
    bool ok = true;
    ok = g_icon_sun.load("/applications/theme-sun.svg", kStatusIcon, kStatusIcon) && ok;
    ok = g_icon_moon.load("/applications/theme-moon.svg", kStatusIcon, kStatusIcon) && ok;
    ok = g_icon_wifi.load("/applications/status-wifi.svg", kStatusIcon, kStatusIcon) && ok;
    ok = g_icon_wifi_off.load("/applications/status-wifi-off.svg", kStatusIcon, kStatusIcon) && ok;
    ok = g_icon_battery.load("/applications/status-battery.svg", 22, 12) && ok;
    ok = g_icon_bolt.load("/applications/status-bolt.svg", 8, 12) && ok;
    return ok;
}

void paint_battery_fill(Surface &s, int bx, int by, Color fg)
{
    const auto &st = status();
    int pct = st.battery_percent;
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    const int pad_x = 3;
    const int pad_y = 3;
    const int inner_w = 14;
    const int inner_h = 6;
    int fill_w = (inner_w * pct + 50) / 100;
    if (pct > 0 && fill_w < 1)
        fill_w = 1;
    Color fill = fg;
    if (pct <= 15 && !st.battery_charging)
        fill = rgb(220, 70, 70);
    if (fill_w > 0)
        s.fill_round(bx + pad_x, by + pad_y, fill_w, inner_h, 1, fill);
}

bool refresh_clock_text()
{
    char next[24];
    hsrc::sdk::time::format_clock(next, sizeof(next));
    if (strcmp(next, g_clock_text) == 0)
        return false;
    strncpy(g_clock_text, next, sizeof(g_clock_text) - 1);
    g_clock_text[sizeof(g_clock_text) - 1] = 0;
    return true;
}

int abs_i(int v)
{
    return v < 0 ? -v : v;
}

int parse_ini_int(const char *s, int fallback)
{
    if (!s || !s[0])
        return fallback;
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
    if (!digits)
        return fallback;
    return neg ? -v : v;
}

void load_dock_prefs()
{
    for (int i = 0; i < APP_COUNT; i++)
        g_pinned[i] = kApps[i].default_pinned;
    g_icon = 52;
    g_mag_enabled = true;
    g_mag_size = kDefaultMagSize;
    g_mag_range = kDefaultMagRange;
    g_mag_lerp = kDefaultMagLerp;

    int fd = (int)hsrc::sdk::open("/etc/os-settings.ini", O_RDONLY);
    if (fd < 0)
        return;
    char buf[kIniBytes];
    memset(buf, 0, sizeof(buf));
    long n = hsrc::sdk::read(fd, buf, sizeof(buf) - 1);
    (void)hsrc::sdk::close(fd);
    if (n <= 0)
        return;

    int start = 0;
    for (long i = 0; i <= n; i++) {
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

        if (strcmp(line, "desktop.dock-size") == 0) {
            if (strcmp(eq, "Small") == 0)
                g_icon = 44;
            else if (strcmp(eq, "Large") == 0)
                g_icon = 60;
            else
                g_icon = 52;
        } else if (strcmp(line, "dock.magnification") == 0) {
            g_mag_enabled = !(strcmp(eq, "Off") == 0 || strcmp(eq, "0") == 0);
        } else if (strcmp(line, "dock.mag-size") == 0) {
            int v = parse_ini_int(eq, kDefaultMagSize);
            if (v < 4)
                v = 4;
            if (v > 48)
                v = 48;
            g_mag_size = v;
        } else if (strcmp(line, "dock.mag-range") == 0) {
            int v = parse_ini_int(eq, kDefaultMagRange);
            if (v < 40)
                v = 40;
            if (v > 280)
                v = 280;
            g_mag_range = v;
        } else if (strcmp(line, "dock.mag-speed") == 0) {
            if (strcmp(eq, "Fast") == 0)
                g_mag_lerp = 2;
            else if (strcmp(eq, "Slow") == 0)
                g_mag_lerp = 5;
            else
                g_mag_lerp = 3;
        } else {
            for (int a = 0; a < APP_COUNT; a++) {
                if (strcmp(line, kApps[a].pin_key) != 0)
                    continue;
                g_pinned[a] = !(strcmp(eq, "Off") == 0 || strcmp(eq, "0") == 0 ||
                                strcmp(eq, "false") == 0);
                break;
            }
        }
    }
}

int app_from_class(const char *cls)
{
    if (!cls || !cls[0])
        return -1;
    for (int i = 0; i < APP_COUNT; i++) {
        if (strcmp(cls, kApps[i].class_name) == 0)
            return i;
    }
    /* legacy settings class */
    if (strcmp(cls, "os-settings") == 0)
        return APP_SETTINGS;
    return -1;
}

void scan_windows()
{
    for (int i = 0; i < APP_COUNT; i++) {
        g_running[i] = false;
        g_minimized[i] = false;

        /* Reuse cached id when still valid — 1 GET instead of id-space probes. */
        int id = g_win_id[i];
        WindowOptions opts;
        if (id >= 0 && hsrc::sdk::window_get(id, opts) &&
            app_from_class(opts.class_name) == i) {
            g_running[i] = true;
            g_minimized[i] = opts.minimized || !opts.visible;
            continue;
        }

        id = hsrc::sdk::window_find_class(kApps[i].class_name);
        if (id < 0 && i == APP_SETTINGS)
            id = hsrc::sdk::window_find_class("os-settings"); /* legacy */
        if (id < 0) {
            g_win_id[i] = -1;
            continue;
        }
        if (!hsrc::sdk::window_get(id, opts)) {
            g_win_id[i] = -1;
            continue;
        }
        g_win_id[i] = id;
        g_running[i] = true;
        g_minimized[i] = opts.minimized || !opts.visible;
    }
}

int bounce_offset(int bounce)
{
    if (bounce <= 0)
        return 0;
    /* Simple bounce curve: peak early, settle. */
    int t = kBounceFrames - bounce;
    if (t < 4)
        return (4 - t) * 3;
    if (t < 8)
        return (t - 4) * 2;
    if (t < 12)
        return (12 - t);
    return 0;
}

int mag_to_px(int mag_fp)
{
    if (mag_fp <= 0)
        return 0;
    return (mag_fp + kMagFP / 2) / kMagFP;
}

/*
 * Continuous macOS-style mag: each icon's target size from |cursor_x - center|.
 * Outside the tray (g_mag_cursor_x < 0) every target collapses to 0.
 * Falloff: smoothstep t^2 * (3-2t) on (1 - dist/range).
 * Size/range/speed come from /etc/os-settings.ini (Desktop & Dock).
 */
void update_mag_targets()
{
    const int range = g_mag_range > 0 ? g_mag_range : kDefaultMagRange;
    const int max_boost = g_mag_enabled ? g_mag_size : 0;
    for (int i = 0; i < g_slot_count; i++) {
        if (g_mag_cursor_x < 0 || max_boost <= 0) {
            g_slots[i].mag_target = 0;
            continue;
        }
        const int cx = slot_local_x(i) + g_icon / 2;
        const int dist = abs_i(g_mag_cursor_x - cx);
        if (dist >= range) {
            g_slots[i].mag_target = 0;
            continue;
        }
        const int u = range - dist;
        const int s_num = u * u * (3 * range - 2 * u);
        const int s_den = range * range * range;
        g_slots[i].mag_target = (max_boost * kMagFP * s_num) / s_den;
    }
}

bool tick_magnification()
{
    bool moved = false;
    int lerp = g_mag_lerp > 0 ? g_mag_lerp : kDefaultMagLerp;
    for (int i = 0; i < g_slot_count; i++) {
        DockSlot &s = g_slots[i];
        int diff = s.mag_target - s.mag;
        if (diff == 0)
            continue;
        int step = diff / lerp;
        if (step == 0)
            step = (diff > 0) ? 1 : -1;
        s.mag += step;
        if ((diff > 0 && s.mag > s.mag_target) ||
            (diff < 0 && s.mag < s.mag_target))
            s.mag = s.mag_target;
        moved = true;
    }
    return moved;
}

int icon_radius(int sz)
{
    int r = sz / 4;
    if (r < 10)
        r = 10;
    if (r > 18)
        r = 18;
    return r;
}

void draw_dock_icon(Surface &s, int ix, int iy, int sz, Color fill)
{
    const int rad = icon_radius(sz);
    /* Light soft shadow — not a black slab. */
    s.fill_round(ix + 1, iy + 2, sz, sz, rad, rgba(0, 0, 0, 22));
    s.fill_round(ix, iy + 1, sz, sz, rad, rgba(0, 0, 0, 12));
    s.fill_round(ix, iy, sz, sz, rad, fill);
}

int compute_tray_width(int slots, bool has_sep)
{
    if (slots <= 0)
        return kDockPad * 2 + 40;
    int w = kDockPad * 2 + slots * g_icon + (slots - 1) * kDockGap;
    if (has_sep)
        w += kSepW + kDockGap;
    return w;
}

const DockSlot *find_prev_slot(const DockSlot *prev, int prev_n, int app)
{
    for (int i = 0; i < prev_n; i++) {
        if (prev[i].app == app)
            return &prev[i];
    }
    return nullptr;
}

void push_slot(const DockSlot *prev, int prev_n, int app, bool pinned)
{
    if (g_slot_count >= kMaxSlots)
        return;
    DockSlot &s = g_slots[g_slot_count++];
    const DockSlot *old = find_prev_slot(prev, prev_n, app);
    s.app = app;
    s.pinned = pinned;
    s.running = g_running[app];
    s.minimized = g_minimized[app];
    s.focused = g_running[app] && g_win_id[app] == g_focus_id && !g_minimized[app];
    s.window_id = g_win_id[app];
    s.bounce = old ? old->bounce : 0;
    s.appear = old ? old->appear : 12;
    s.mag = old ? old->mag : 0;
    s.mag_target = old ? old->mag_target : 0;
}

void rebuild_slots()
{
    DockSlot prev[kMaxSlots];
    int prev_n = g_slot_count;
    for (int i = 0; i < prev_n; i++)
        prev[i] = g_slots[i];

    g_slot_count = 0;
    g_sep_after = -1;

    for (int i = 0; i < APP_COUNT; i++) {
        if (g_pinned[i])
            push_slot(prev, prev_n, i, true);
    }

    bool any_unpinned = false;
    for (int i = 0; i < APP_COUNT; i++) {
        if (!g_pinned[i] && g_running[i])
            any_unpinned = true;
    }
    if (any_unpinned && g_slot_count > 0)
        g_sep_after = g_slot_count - 1;

    for (int i = 0; i < APP_COUNT; i++) {
        if (!g_pinned[i] && g_running[i])
            push_slot(prev, prev_n, i, false);
    }

    g_tray_target = compute_tray_width(g_slot_count, g_sep_after >= 0);
}

void tick_animations()
{
    for (int i = 0; i < g_slot_count; i++) {
        if (g_slots[i].bounce > 0)
            g_slots[i].bounce--;
        if (g_slots[i].appear > 0)
            g_slots[i].appear--;
    }
    update_mag_targets();
    (void)tick_magnification();
    if (g_tray_w < g_tray_target) {
        int step = (g_tray_target - g_tray_w + 3) / 4;
        if (step < 2)
            step = 2;
        g_tray_w += step;
        if (g_tray_w > g_tray_target)
            g_tray_w = g_tray_target;
    } else if (g_tray_w > g_tray_target) {
        int step = (g_tray_w - g_tray_target + 3) / 4;
        if (step < 2)
            step = 2;
        g_tray_w -= step;
        if (g_tray_w < g_tray_target)
            g_tray_w = g_tray_target;
    }
}

bool animating()
{
    if (g_tray_w != g_tray_target)
        return true;
    for (int i = 0; i < g_slot_count; i++) {
        if (g_slots[i].bounce > 0 || g_slots[i].appear > 0)
            return true;
        if (g_slots[i].mag != g_slots[i].mag_target)
            return true;
    }
    return false;
}

void paint_desktop()
{
    Surface &s = g_desktop.surface();
    s.clear(kTransparent);
    s.text_centered((int)s.width() / 2, (int)s.height() / 2 - 10, "HSRC OS", kWatermark, 2);
    s.text_centered((int)s.width() / 2, (int)s.height() / 2 + 24, "desktop",
                    rgba(255, 255, 255, 36), 1);
    g_desktop.damage();
}

void paint_menubar()
{
    Surface &s = g_menubar.surface();
    s.clear(kTransparent);
    s.fill(0, 0, g_sw, 1, rgba(255, 255, 255, 96));
    s.fill(0, kMenubarH - 1, g_sw, 1, rgba(0, 0, 0, 48));

    const Color fg = menubar_fg();
    const Color hover_fg = kMenubarAccent;

    s.fill_round(10, 5, 16, 16, 8,
                 theme_mode() == ThemeMode::Dark ? rgb(220, 220, 225) : rgb(30, 30, 32));
    const int text_y = (kMenubarH - Surface::text_height(1)) / 2;
    s.text(32, text_y, "HSRC", fg, 1);

    s.text(kMenuSettingsX, text_y, "Settings",
           g_menu_hover == 0 ? hover_fg : fg, 1);
    s.text(kMenuSystemInfoX, text_y, "System Information",
           g_menu_hover == 1 ? hover_fg : fg, 1);

    /* Right cluster (macOS-like): theme · wifi · battery% · clock */
    if (!g_clock_text[0])
        (void)refresh_clock_text();

    const auto &st = status();
    char pct_txt[8];
    format_percent(pct_txt, sizeof(pct_txt), st.battery_percent);
    const int pct_w = text_width(pct_txt);
    const int bat_w = g_icon_battery.valid() ? g_icon_battery.width() : 22;
    const int bat_h = g_icon_battery.valid() ? g_icon_battery.height() : 12;
    const int clock_w = text_width(g_clock_text);

    int x = g_sw - kStatusRightPad;

    x -= clock_w;
    g_slot_clock = { x - 4, 0, clock_w + 8, kMenubarH };
    const int clock_x = x;
    x -= kStatusGap;

    const int bat_block = bat_w + 4 + pct_w;
    x -= bat_block;
    g_slot_battery = { x - 4, 0, bat_block + 8, kMenubarH };
    const int bat_x = x;
    const int bat_y = (kMenubarH - bat_h) / 2;
    x -= kStatusGap;

    x -= kStatusIcon;
    g_slot_wifi = { x - 4, 0, kStatusIcon + 8, kMenubarH };
    const int wifi_x = x;
    const int wifi_y = (kMenubarH - kStatusIcon) / 2;
    x -= kStatusGap;

    x -= kStatusIcon;
    g_slot_theme = { x - 4, 0, kStatusIcon + 8, kMenubarH };
    const int theme_x = x;
    const int theme_y = (kMenubarH - kStatusIcon) / 2;

    Color theme_tint = (g_status_hover == 0) ? hover_fg : fg;
    Color wifi_tint = (g_status_hover == 1) ? hover_fg : fg;
    Color bat_tint = (g_status_hover == 2) ? hover_fg : fg;
    Color clock_tint = (g_status_hover == 3) ? hover_fg : fg;

    if (theme_mode() == ThemeMode::Dark) {
        if (g_icon_sun.valid())
            g_icon_sun.blit(s, theme_x, theme_y, theme_tint);
        else
            s.fill_round(theme_x + 2, theme_y + 2, 12, 12, 6, theme_tint);
    } else {
        if (g_icon_moon.valid())
            g_icon_moon.blit(s, theme_x, theme_y, theme_tint);
        else
            s.fill_round(theme_x + 2, theme_y + 2, 12, 12, 6, theme_tint);
    }

    if (st.wifi_connected && st.wifi_bars > 0) {
        if (g_icon_wifi.valid())
            g_icon_wifi.blit(s, wifi_x, wifi_y, wifi_tint);
        else
            s.fill_round(wifi_x + 2, wifi_y + 2, 12, 12, 3, wifi_tint);
    } else if (g_icon_wifi_off.valid()) {
        g_icon_wifi_off.blit(s, wifi_x, wifi_y, wifi_tint);
    } else {
        s.rect(wifi_x + 2, wifi_y + 2, 12, 12, wifi_tint, 1);
    }

    if (g_icon_battery.valid()) {
        paint_battery_fill(s, bat_x, bat_y, bat_tint);
        g_icon_battery.blit(s, bat_x, bat_y, bat_tint);
        if (st.battery_charging && g_icon_bolt.valid())
            g_icon_bolt.blit(s, bat_x + 7, bat_y, bat_tint);
    } else {
        s.rect(bat_x, bat_y, bat_w - 3, bat_h, bat_tint, 1);
        paint_battery_fill(s, bat_x, bat_y, bat_tint);
    }
    s.text(bat_x + bat_w + 4, text_y, pct_txt, bat_tint, 1);
    s.text(clock_x, text_y, g_clock_text, clock_tint, 1);

    /* Full menubar width is acrylic — prefer band damage over full-screen escalate. */
    g_menubar.damage(0, 0, g_sw, kMenubarH);
}

int slot_local_x(int index)
{
    const int tray0 = (g_dock_w - g_tray_w) / 2;
    int x = tray0 + kDockPad;
    for (int i = 0; i < index; i++) {
        x += g_icon + kDockGap;
        if (g_sep_after == i)
            x += kSepW + kDockGap;
    }
    return x;
}

void paint_dock()
{
    Surface &s = g_dock.surface();
    s.clear(kTransparent);

    const int tray0 = (g_dock_w - g_tray_w) / 2;
    const int tray_y = kDockMagPad;

    /* Light glass tray only — mag headroom above stays fully clear. */
    s.fill_round(tray0, tray_y, g_tray_w, kDockTrayH, kDockRad, rgba(255, 255, 255, 48));
    s.fill_round(tray0 + 1, tray_y + 1, g_tray_w - 2, kDockTrayH - 2, kDockRad - 1,
                 rgba(255, 255, 255, 22));
    s.fill(tray0 + 14, tray_y + 1, g_tray_w - 28, 1, rgba(255, 255, 255, 90));

    if (g_sep_after >= 0 && g_sep_after + 1 < g_slot_count) {
        int sx = slot_local_x(g_sep_after) + g_icon + kDockGap / 2 + 2;
        s.fill(sx, tray_y + 14, 2, kDockTrayH - 28, rgba(255, 255, 255, 70));
    }

    const int base_y = tray_y + (kDockTrayH - g_icon) / 2;

    for (int i = 0; i < g_slot_count; i++) {
        const DockSlot &slot = g_slots[i];
        if (slot.app < 0 || slot.app >= APP_COUNT)
            continue;
        const AppDef &app = kApps[slot.app];
        const int base_x = slot_local_x(i);
        const int boost = mag_to_px(slot.mag);
        const int appear_shrink = slot.appear > 0 ? slot.appear : 0;
        int sz = g_icon + boost - appear_shrink;
        if (sz < 24)
            sz = 24;
        const int lift = bounce_offset(slot.bounce) + boost / 2;
        const int ix = base_x - (sz - g_icon) / 2;
        const int iy = base_y - lift - (sz - g_icon) / 2;

        draw_dock_icon(s, ix, iy, sz, app.color);

        const int tw = text_width(app.label);
        s.text(ix + (sz - tw) / 2, iy + sz / 2 - 3, app.label, kDockFg, 1);

        if (slot.running) {
            const int dx = base_x + g_icon / 2 - 2;
            const int dy = tray_y + kDockTrayH - 10;
            Color dot = slot.minimized ? kDotHidden : kDotLive;
            if (slot.focused)
                dot = rgb(255, 255, 255);
            s.fill_round(dx, dy, 5, 5, 2, dot);
        }
    }

    /* Dock surface is tall (mag pad); damage painted tray + mag headroom only. */
    g_dock.damage(tray0, 0, g_tray_w, kDockH);
}

/* True if pointer is inside the painted tray; writes dock-local coords. */
bool dock_tray_contains(int x, int y, int *out_lx, int *out_ly)
{
    const int lx = x - g_dock_x;
    const int ly = y - g_dock_y;
    const int tray0 = (g_dock_w - g_tray_w) / 2;
    const int tray_y = kDockMagPad;

    if (ly < tray_y || ly >= tray_y + kDockTrayH)
        return false;
    if (lx < tray0 || lx >= tray0 + g_tray_w)
        return false;
    if (out_lx)
        *out_lx = lx;
    if (out_ly)
        *out_ly = ly;
    return true;
}

/* Nearest icon center to dock-local X (click target). */
int dock_nearest_slot(int lx)
{
    if (g_slot_count <= 0)
        return -1;
    int best = 0;
    int best_d = abs_i(lx - (slot_local_x(0) + g_icon / 2));
    for (int i = 1; i < g_slot_count; i++) {
        const int cx = slot_local_x(i) + g_icon / 2;
        const int d = abs_i(lx - cx);
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

int menubar_hit(int x, int y)
{
    if (y < 0 || y >= kMenubarH)
        return -1;

    const int settings_w = text_width("Settings");
    if (x >= kMenuSettingsX && x < kMenuSettingsX + settings_w)
        return 0;

    const int info_w = text_width("System Information");
    if (x >= kMenuSystemInfoX && x < kMenuSystemInfoX + info_w)
        return 1;

    return -1;
}

int status_hit(int x, int y)
{
    if (y < 0 || y >= kMenubarH)
        return -1;
    if (x >= g_slot_theme.x && x < g_slot_theme.x + g_slot_theme.w)
        return 0;
    if (x >= g_slot_wifi.x && x < g_slot_wifi.x + g_slot_wifi.w)
        return 1;
    if (x >= g_slot_battery.x && x < g_slot_battery.x + g_slot_battery.w)
        return 2;
    if (x >= g_slot_clock.x && x < g_slot_clock.x + g_slot_clock.w)
        return 3;
    return -1;
}

void dock_activate_slot(int slot_i)
{
    if (slot_i < 0 || slot_i >= g_slot_count)
        return;

    DockSlot &slot = g_slots[slot_i];
    if (slot.app < 0 || slot.app >= APP_COUNT)
        return;

    const AppDef &app = kApps[slot.app];
    slot.bounce = kBounceFrames;

    if (slot.app == APP_SETTINGS && !slot.running) {
        if (g_spawn_cool[slot.app] > 0 || g_spawn_fail[slot.app] > 0)
            return;
        note_spawn_attempt(slot.app);
        (void)hsrc::sdk::settings::open();
        return;
    }

    if (!slot.running || slot.window_id < 0) {
        if (g_spawn_cool[slot.app] > 0 || g_spawn_fail[slot.app] > 0)
            return;
        if (app.path && app.path[0]) {
            note_spawn_attempt(slot.app);
            (void)hsrc::sdk::process::spawn(app.path);
        }
        return;
    }

    WindowOptions opts;
    if (!hsrc::sdk::window_get(slot.window_id, opts)) {
        /* Stale cache — rescan soon; do not spawn-storm on GET races. */
        g_win_id[slot.app] = -1;
        g_scan_tick = kScanEvery;
        if (g_spawn_cool[slot.app] > 0 || g_spawn_fail[slot.app] > 0)
            return;
        if (app.path && app.path[0]) {
            note_spawn_attempt(slot.app);
            (void)hsrc::sdk::process::spawn(app.path);
        }
        return;
    }

    const bool hidden = opts.minimized || !opts.visible;
    const bool focused = (g_focus_id == slot.window_id) && !hidden;

    if (hidden) {
        opts.visible = true;
        opts.minimized = false;
        (void)hsrc::sdk::window_set(slot.window_id, opts);
        (void)hsrc::sdk::syscall2(SYS_WM_SHOW, slot.window_id, 1);
        (void)hsrc::sdk::syscall1(SYS_WM_FOCUS, slot.window_id);
        (void)hsrc::sdk::syscall1(SYS_GX_DAMAGE, slot.window_id);
        return;
    }

    if (focused) {
        /* Apple-like: click focused dock icon → minimize / hide. */
        opts.minimized = true;
        opts.visible = true;
        (void)hsrc::sdk::window_set(slot.window_id, opts);
        return;
    }

    opts.visible = true;
    opts.minimized = false;
    (void)hsrc::sdk::window_set(slot.window_id, opts);
    (void)hsrc::sdk::syscall2(SYS_WM_SHOW, slot.window_id, 1);
    (void)hsrc::sdk::syscall1(SYS_WM_FOCUS, slot.window_id);
    (void)hsrc::sdk::syscall1(SYS_GX_DAMAGE, slot.window_id);
}

bool build_ui()
{
    if (!hsrc::sdk::set_wallpaper_default() &&
        !hsrc::sdk::set_wallpaper_color(kDesktop))
        return false;

    (void)hsrc::sdk::time::init();
    (void)load_status_icons();
    (void)refresh_theme();
    (void)refresh_status();
    (void)refresh_clock_text();

    /* Apply timezone / clock format from ini into the shared time page. */
    {
        int fd = (int)hsrc::sdk::open("/etc/os-settings.ini", O_RDONLY);
        if (fd >= 0) {
            char buf[kIniBytes];
            memset(buf, 0, sizeof(buf));
            long n = hsrc::sdk::read(fd, buf, sizeof(buf) - 1);
            (void)hsrc::sdk::close(fd);
            int start = 0;
            for (long i = 0; i <= n; i++) {
                if (i < n && buf[i] != '\n' && buf[i] != '\r')
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
                if (strcmp(line, "datetime.timezone") == 0) {
                    int off = 0;
                    if (hsrc::sdk::time::parse_timezone_label(eq, &off))
                        (void)hsrc::sdk::time::set_timezone(off, eq);
                } else if (strcmp(line, "datetime.clock") == 0) {
                    (void)hsrc::sdk::time::set_hour12(strcmp(eq, "12-hour") == 0);
                }
            }
            (void)refresh_clock_text();
        }
    }

    load_dock_prefs();
    scan_windows();
    rebuild_slots();
    g_tray_w = g_tray_target;

    constexpr int kMarkW = 360;
    constexpr int kMarkH = 100;
    WindowOptions desktop_opts;
    desktop_opts.x = (g_sw - kMarkW) / 2;
    desktop_opts.y = (g_sh - kMarkH) / 2 - 20;
    desktop_opts.w = kMarkW;
    desktop_opts.h = kMarkH;
    desktop_opts.background = true;
    desktop_opts.no_drag = true;
    desktop_opts.no_title = true;
    desktop_opts.alpha = true;
    desktop_opts.accept_focus = false;
    desktop_opts.set_title("desktop");
    desktop_opts.set_class_name("shell.desktop");
    if (!g_desktop.create(desktop_opts))
        return false;

    WindowOptions menubar_opts;
    menubar_opts.x = 0;
    menubar_opts.y = 0;
    menubar_opts.w = g_sw;
    menubar_opts.h = kMenubarH;
    menubar_opts.background = true;
    menubar_opts.no_drag = true;
    menubar_opts.no_title = true;
    menubar_opts.acrylic = true;
    menubar_opts.alpha = true;
    menubar_opts.accept_focus = false;
    menubar_opts.topmost = true;
    menubar_opts.set_title("menubar");
    menubar_opts.set_class_name("shell.menubar");
    if (!g_menubar.create(menubar_opts))
        return false;

    /* Max tray for all apps + separator — surface stays fixed (no resize thrash). */
    g_dock_w = compute_tray_width(APP_COUNT, true) + 40;
    g_dock_x = (g_sw - g_dock_w) / 2;
    g_dock_y = g_sh - kDockH - 14;

    WindowOptions dock_opts;
    dock_opts.x = g_dock_x;
    dock_opts.y = g_dock_y;
    dock_opts.w = g_dock_w;
    dock_opts.h = kDockH;
    dock_opts.radius = 0;
    dock_opts.rounded = false; /* tray rounds are painted; window round = black slab */
    dock_opts.no_drag = true;
    dock_opts.no_title = true;
    dock_opts.background = true;
    /* No acrylic: compositor tint filled the whole window incl. mag pad (black slab). */
    dock_opts.acrylic = false;
    dock_opts.alpha = true;
    dock_opts.accept_focus = false;
    dock_opts.topmost = true;
    dock_opts.set_title("dock");
    dock_opts.set_class_name("shell.dock");
    if (!g_dock.create(dock_opts))
        return false;

    paint_desktop();
    paint_menubar();
    paint_dock();
    return true;
}

} // namespace

extern "C" void mke_main(void)
{
    ScreenInfo info{};
    if (!hsrc::sdk::screen_info(info) || info.width == 0 || info.height == 0) {
        for (;;)
            hsrc::sdk::this_thread::sleep_for(1000u);
    }

    g_sw = (int)info.width;
    g_sh = (int)info.height;

    for (int i = 0; i < APP_COUNT; i++)
        g_pinned[i] = kApps[i].default_pinned;

    if (!g_gx.create_shell()) {
        for (;;)
            hsrc::sdk::this_thread::sleep_for(1000u);
    }

    if (!build_ui()) {
        (void)hsrc::sdk::set_wallpaper_color(rgb(160, 30, 30));
        (void)g_gx.begin_scene();
        (void)g_gx.end_scene();
        (void)g_gx.present();
        for (;;)
            hsrc::sdk::this_thread::sleep_for(1000u);
    }

    (void)g_gx.begin_scene();
    (void)g_gx.end_scene();
    (void)g_gx.present();

    for (;;) {
        bool dirty_dock = false;
        bool dirty_menu = false;

        if (++g_theme_poll >= kThemePollEvery) {
            g_theme_poll = 0;
            if (refresh_theme())
                dirty_menu = true;
        }
        if (++g_status_poll >= kStatusPollEvery) {
            g_status_poll = 0;
            if (refresh_status())
                dirty_menu = true;
        }
        /* Clock text from shared time page + RDTSC — no gettime syscall. */
        if (refresh_clock_text())
            dirty_menu = true;

        for (int i = 0; i < APP_COUNT; i++) {
            if (g_spawn_cool[i] > 0)
                g_spawn_cool[i]--;
            if (g_spawn_fail[i] > 0)
                g_spawn_fail[i]--;
        }

        g_scan_tick++;
        if (g_scan_tick >= kScanEvery) {
            g_scan_tick = 0;
            bool prev_run[APP_COUNT];
            bool prev_min[APP_COUNT];
            bool prev_pin[APP_COUNT];
            for (int i = 0; i < APP_COUNT; i++) {
                prev_run[i] = g_running[i];
                prev_min[i] = g_minimized[i];
                prev_pin[i] = g_pinned[i];
            }
            int prev_focus = g_focus_id;
            scan_windows();

            for (int i = 0; i < APP_COUNT; i++) {
                if (g_running[i]) {
                    g_spawn_pending[i] = false;
                    g_spawn_fail[i] = 0;
                } else if (g_spawn_pending[i] && g_spawn_cool[i] == 0) {
                    /* Cool expired, still no window → OOM/crash; back off. */
                    g_spawn_fail[i] = kSpawnFailFrames;
                    g_spawn_pending[i] = false;
                }
            }

            g_pin_tick++;
            if (g_pin_tick >= kPinPollEvery / kScanEvery) {
                g_pin_tick = 0;
                int prev_icon = g_icon;
                int prev_mag = g_mag_size;
                int prev_range = g_mag_range;
                int prev_lerp = g_mag_lerp;
                bool prev_en = g_mag_enabled;
                load_dock_prefs();
                if (prev_icon != g_icon || prev_mag != g_mag_size ||
                    prev_range != g_mag_range || prev_lerp != g_mag_lerp ||
                    prev_en != g_mag_enabled)
                    dirty_dock = true;
            }

            rebuild_slots();
            for (int i = 0; i < APP_COUNT; i++) {
                if (prev_run[i] != g_running[i] || prev_min[i] != g_minimized[i] ||
                    prev_pin[i] != g_pinned[i])
                    dirty_dock = true;
            }
            if (prev_focus != g_focus_id)
                dirty_dock = true;
        }

        /* Flush UI before blocking — never spin with timeout 0. */
        if (dirty_menu) {
            (void)g_gx.begin_scene();
            paint_menubar();
            (void)g_gx.end_scene();
            (void)g_gx.present();
            dirty_menu = false;
        }
        if (dirty_dock) {
            (void)g_gx.begin_scene();
            paint_dock();
            (void)g_gx.end_scene();
            (void)g_gx.present();
            dirty_dock = false;
        }

        /*
         * Shell watches all input (win=-1) for dock/menubar hover.
         * Short timeout only while animating / clock / scan.
         */
        uint32_t wait_to = 50u; /* ~0.5s clock/scan cadence when idle */
        if (animating() || g_hover >= 0 || g_menu_hover >= 0 || g_status_hover >= 0 ||
            g_mag_cursor_x >= 0)
            wait_to = 1u;

        Input in = g_gx.wait(wait_to);

        {
            if (in.focus_id != g_focus_id) {
                g_focus_id = in.focus_id;
                for (int i = 0; i < g_slot_count; i++) {
                    DockSlot &s = g_slots[i];
                    s.focused = s.running && s.window_id == g_focus_id && !s.minimized;
                }
                dirty_dock = true;
            }

            /*
             * Shell chrome only owns the pointer when hit-test says so.
             * Otherwise app windows would click-through to dock/menubar.
             */
            const bool shell_hit =
                in.hit_id == g_menubar.id() ||
                in.hit_id == g_dock.id() ||
                in.hit_id == g_desktop.id();

            const int menu_hover = shell_hit ? menubar_hit(in.mouse_x, in.mouse_y) : -1;
            const int status_hover = shell_hit ? status_hit(in.mouse_x, in.mouse_y) : -1;

            int lx = 0;
            const bool in_tray = shell_hit &&
                                 dock_tray_contains(in.mouse_x, in.mouse_y, &lx, nullptr);
            const int mag_x = in_tray ? lx : -1;
            const int hover = in_tray ? dock_nearest_slot(lx) : -1;

            if (menu_hover != g_menu_hover || status_hover != g_status_hover) {
                g_menu_hover = menu_hover;
                g_status_hover = status_hover;
                dirty_menu = true;
            }
            /* Mag targets track cursor; paint only when mag/bounce actually moves. */
            g_mag_cursor_x = mag_x;
            g_hover = hover;

            const uint8_t pressed = (uint8_t)(in.buttons & ~g_prev_buttons);
            if (shell_hit && (pressed & UGX_BTN_LEFT)) {
                if (g_status_hover == 0) {
                    if (toggle_theme())
                        dirty_menu = true;
                } else if (g_status_hover == 1) {
                    (void)hsrc::sdk::settings::open_deeplink("settings://general");
                } else if (g_status_hover == 2) {
                    (void)hsrc::sdk::settings::open_deeplink("settings://about");
                } else if (g_status_hover == 3) {
                    (void)hsrc::sdk::settings::open_deeplink("settings://date-time");
                } else if (g_menu_hover == 0) {
                    (void)hsrc::sdk::settings::open();
                } else if (g_menu_hover == 1) {
                    (void)hsrc::sdk::settings::open_deeplink("settings://about");
                }

                if (g_hover >= 0) {
                    dock_activate_slot(g_hover);
                    dirty_dock = true;
                    g_scan_tick = kScanEvery; /* rescan soon */
                }
            }
            g_prev_buttons = in.buttons;
        }

        tick_animations();
        if (animating())
            dirty_dock = true;

        if (dirty_menu) {
            (void)g_gx.begin_scene();
            paint_menubar();
            (void)g_gx.end_scene();
            (void)g_gx.present();
        }
        if (dirty_dock) {
            (void)g_gx.begin_scene();
            paint_dock();
            (void)g_gx.end_scene();
            (void)g_gx.present();
        }
    }
}
