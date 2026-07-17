#include <user/mke.h>
#include <kernel/syscall.h>
#include <user/sdk/fs.hpp>
#include <user/sdk/gfx.hpp>
#include <user/sdk/settings.hpp>
#include <user/sdk/syscall.hpp>
#include <user/string.h>

namespace {

using hsrc::sdk::ChromeHit;
using hsrc::sdk::Color;
using hsrc::sdk::Input;
using hsrc::sdk::ScreenInfo;
using hsrc::sdk::Surface;
using hsrc::sdk::Window;
using hsrc::sdk::WindowOptions;
using hsrc::sdk::kChromeTitleH;
using hsrc::sdk::settings::theme;
using hsrc::sdk::settings::refresh_theme;

constexpr int kWinW = 860;
constexpr int kWinH = 520;
constexpr int kSidebarW = 220;
constexpr int kHeaderH = 52;
constexpr int kSidebarTop = 68;
constexpr int kSidebarItemH = 34;
constexpr int kRowH = 38;
constexpr int kRowGap = 10;
constexpr int kSectionH = 30;
constexpr int kContentX = kSidebarW + 24;
constexpr int kContentW = kWinW - kContentX - 24;
constexpr int kRowsStartY = 92;

constexpr int kThemePollEvery = 96;

constexpr const char *kIniDir = "/etc";
constexpr const char *kIniPath = "/etc/os-settings.ini";
constexpr const char *kDeepLinkPath = "/run/settings.deeplink";
constexpr size_t kDeepLinkBytes = 128;
constexpr size_t kIniBytes = 2048;

enum CategoryIndex {
    CAT_GENERAL = 0,
    CAT_KEYBOARD,
    CAT_MOUSE,
    CAT_DISPLAY,
    CAT_NETWORK,
    CAT_DESKTOP_DOCK,
    CAT_STORAGE,
    CAT_DATE_TIME,
    CAT_SOUND,
    CAT_ABOUT,
    CAT_COUNT
};

enum TargetKind {
    TARGET_SETTING = 0,
    TARGET_SECTION = 1,
};

struct Category {
    const char *id;
    const char *label;
};

struct CycleSetting {
    const char *key;
    int category;
    const char *label;
    const char *const *choices;
    int choice_count;
    int current;
};

struct ClickTarget {
    int y;
    int h;
    int kind;
    int index;
};

constexpr Category kCategories[CAT_COUNT] = {
    { "general", "General" },
    { "keyboard", "Keyboard" },
    { "mouse", "Mouse" },
    { "display", "Display" },
    { "network", "Network" },
    { "desktop-dock", "Desktop & Dock" },
    { "storage", "Storage" },
    { "date-time", "Date & Time" },
    { "sound", "Sound" },
    { "about", "About" },
};

constexpr const char *kAppearanceChoices[] = { "Light", "Dark", "Auto" };
constexpr const char *kAccentChoices[] = { "Blue", "Graphite", "Forest" };
constexpr const char *kMotionChoices[] = { "Fast", "Reduced" };
constexpr const char *kKeyboardLayoutChoices[] = { "US", "TR", "UK" };
constexpr const char *kRepeatChoices[] = { "Normal", "Fast", "Slow" };
constexpr const char *kOnOffChoices[] = { "On", "Off" };
constexpr const char *kTrackingChoices[] = { "Slow", "Normal", "Fast" };
constexpr const char *kIpv4Choices[] = { "DHCP", "Static" };
constexpr const char *kHostChoices[] = { "hsrc-os", "studio", "lab-box" };
constexpr const char *kDockChoices[] = { "Small", "Medium", "Large" };
constexpr const char *kWallpaperChoices[] = { "Cover", "Center", "Stretch" };
constexpr const char *kCleanupChoices[] = { "Manual", "Weekly", "Monthly" };
constexpr const char *kClockChoices[] = { "24-hour", "12-hour" };
constexpr const char *kTimezoneChoices[] = { "UTC+3", "UTC", "UTC+1" };
constexpr const char *kOutputChoices[] = { "Speakers", "Headphones", "Muted" };

CycleSetting g_settings[] = {
    { "general.appearance", CAT_GENERAL, "Appearance", kAppearanceChoices, 3, 0 },
    { "general.accent", CAT_GENERAL, "Accent Color", kAccentChoices, 3, 0 },
    { "general.motion", CAT_GENERAL, "Animation Speed", kMotionChoices, 2, 0 },
    { "keyboard.layout", CAT_KEYBOARD, "Keyboard Layout", kKeyboardLayoutChoices, 3, 0 },
    { "keyboard.repeat", CAT_KEYBOARD, "Key Repeat", kRepeatChoices, 3, 0 },
    { "keyboard.shortcuts", CAT_KEYBOARD, "Full Keyboard Access", kOnOffChoices, 2, 0 },
    { "mouse.tracking", CAT_MOUSE, "Tracking Speed", kTrackingChoices, 3, 0 },
    { "mouse.natural", CAT_MOUSE, "Natural Scroll", kOnOffChoices, 2, 1 },
    { "network.ipv4", CAT_NETWORK, "IPv4 Mode", kIpv4Choices, 2, 0 },
    { "network.hostname", CAT_NETWORK, "Hostname", kHostChoices, 3, 0 },
    { "desktop.dock-size", CAT_DESKTOP_DOCK, "Dock Size", kDockChoices, 3, 1 },
    { "desktop.wallpaper", CAT_DESKTOP_DOCK, "Wallpaper Fit", kWallpaperChoices, 3, 0 },
    { "storage.cleanup", CAT_STORAGE, "Cleanup Schedule", kCleanupChoices, 3, 0 },
    { "datetime.clock", CAT_DATE_TIME, "Clock Format", kClockChoices, 2, 0 },
    { "datetime.timezone", CAT_DATE_TIME, "Timezone", kTimezoneChoices, 3, 0 },
    { "sound.output", CAT_SOUND, "Output Device", kOutputChoices, 3, 0 },
    { "sound.alerts", CAT_SOUND, "System Alerts", kOnOffChoices, 2, 0 },
};

constexpr int kSettingCount = (int)(sizeof(g_settings) / sizeof(g_settings[0]));

Window g_win;
WindowOptions g_win_opts;
ScreenInfo g_screen{};
Input g_prev_input{};
bool g_dirty = true;
int g_theme_poll = 0;
bool g_running = true;
int g_active_category = CAT_GENERAL;
int g_hover_sidebar = -1;
int g_hover_setting = -1;
int g_deeplink_cooldown = 0;
bool g_section_open[CAT_COUNT];
ClickTarget g_targets[24];
int g_target_count = 0;

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
        char ch[2];
        ch[0] = tmp[i];
        ch[1] = 0;
        append_text(dst, dst_size, ch);
    }
}

int text_width(const char *s, int scale = 1)
{
    return Surface::text_width(s, scale);
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

void make_resolution_text(char *out, size_t out_size)
{
    out[0] = 0;
    append_uint(out, out_size, g_screen.width);
    append_text(out, out_size, " x ");
    append_uint(out, out_size, g_screen.height);
}

void make_color_depth_text(char *out, size_t out_size)
{
    out[0] = 0;
    append_uint(out, out_size, g_screen.bpp);
    append_text(out, out_size, "-bit");
}

void draw_row(Surface &s, int y, const char *label, const char *value, bool interactive, bool hover)
{
    const auto &t = theme();
    Color row_bg = interactive ? t.accent_soft : t.card;
    s.fill_round(kContentX, y, kContentW, kRowH, 8, row_bg);
    s.rect(kContentX, y, kContentW, kRowH, hover ? t.accent : t.border, hover ? 2 : 1);
    s.text(kContentX + 14, y + 12, label, t.text, 1);
    int value_x = kContentX + kContentW - 14 - text_width(value, 1);
    if (value_x < kContentX + 220)
        value_x = kContentX + 220;
    s.text(value_x, y + 12, value, interactive ? t.accent : t.text_dim, 1);
}

int draw_section_header(Surface &s, int y, const char *title, bool open, bool register_hit)
{
    const auto &t = theme();
    const char *mark = open ? "v " : "> ";
    s.fill_round(kContentX, y, kContentW, kSectionH, 8, t.hover);
    s.text(kContentX + 12, y + 8, mark, t.text_dim, 1);
    s.text(kContentX + 28, y + 8, title, t.text, 1);
    if (register_hit)
        register_target(y, kSectionH, TARGET_SECTION, g_active_category);
    return y + kSectionH + 8;
}

void save_settings()
{
    (void)hsrc::sdk::mkdir(kIniDir, 0755);
    int fd = (int)hsrc::sdk::open(kIniPath, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return;

    for (int i = 0; i < kSettingCount; i++) {
        const char *value = g_settings[i].choices[g_settings[i].current];
        (void)hsrc::sdk::write(fd, g_settings[i].key, strlen(g_settings[i].key));
        (void)hsrc::sdk::write(fd, "=", 1);
        (void)hsrc::sdk::write(fd, value, strlen(value));
        (void)hsrc::sdk::write(fd, "\n", 1);
    }

    (void)hsrc::sdk::close(fd);
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
            for (int c = 0; c < g_settings[s].choice_count; c++) {
                if (strcmp(eq, g_settings[s].choices[c]) == 0) {
                    g_settings[s].current = c;
                    break;
                }
            }
            break;
        }
    }
}

int category_from_id(const char *id)
{
    if (!id || !id[0])
        return CAT_GENERAL;
    if (strcmp(id, "system-information") == 0)
        return CAT_ABOUT;
    for (int i = 0; i < CAT_COUNT; i++) {
        if (strcmp(id, kCategories[i].id) == 0)
            return i;
    }
    return CAT_GENERAL;
}

void poll_deeplink()
{
    if (g_deeplink_cooldown > 0) {
        g_deeplink_cooldown--;
        return;
    }

    int fd = (int)hsrc::sdk::open(kDeepLinkPath, O_RDONLY);
    if (fd < 0) {
        g_deeplink_cooldown = 8;
        return;
    }

    char buf[kDeepLinkBytes];
    memset(buf, 0, sizeof(buf));
    long nread = hsrc::sdk::read(fd, buf, sizeof(buf) - 1);
    (void)hsrc::sdk::close(fd);
    unlink_deeplink();
    g_deeplink_cooldown = 4;

    if (nread <= 0 || buf[0] == 0)
        return;

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
    g_dirty = true;
}

bool refresh_window_options()
{
    return g_win.get_options(g_win_opts);
}

int sidebar_hit(int lx, int ly)
{
    if (lx < 8 || lx >= kSidebarW - 8 || ly < kSidebarTop)
        return -1;
    for (int i = 0; i < CAT_COUNT; i++) {
        int y = kSidebarTop + i * kSidebarItemH;
        if (ly >= y && ly < y + kSidebarItemH)
            return i;
    }
    return -1;
}

const ClickTarget *target_hit(int lx, int ly)
{
    if (lx < kContentX || lx >= kContentX + kContentW)
        return nullptr;
    for (int i = 0; i < g_target_count; i++) {
        if (ly >= g_targets[i].y && ly < g_targets[i].y + g_targets[i].h)
            return &g_targets[i];
    }
    return nullptr;
}

void cycle_setting(int idx)
{
    if (idx < 0 || idx >= kSettingCount)
        return;
    g_settings[idx].current++;
    if (g_settings[idx].current >= g_settings[idx].choice_count)
        g_settings[idx].current = 0;
    save_settings();
    (void)refresh_theme();
    g_dirty = true;
}

void draw_sidebar(Surface &s)
{
    const auto &t = theme();
    s.fill(0, kChromeTitleH, kSidebarW, kWinH - kChromeTitleH, t.sidebar);
    s.fill(kSidebarW - 1, kChromeTitleH, 1, kWinH - kChromeTitleH, t.border);
    s.text(18, kChromeTitleH + 14, "System Settings", t.text, 1);
    s.text(18, kChromeTitleH + 32, "Ring-3 control center", t.text_dim, 1);

    for (int i = 0; i < CAT_COUNT; i++) {
        int y = kSidebarTop + i * kSidebarItemH;
        bool selected = (i == g_active_category);
        bool hover = (i == g_hover_sidebar);
        if (selected)
            s.fill_round(10, y + 2, kSidebarW - 20, kSidebarItemH - 4, 8, t.accent_soft);
        else if (hover)
            s.fill_round(10, y + 2, kSidebarW - 20, kSidebarItemH - 4, 8, t.hover);
        Color label = selected ? t.accent : t.text;
        s.text(24, y + 10, kCategories[i].label, label, 1);
    }
}

void draw_page_title(Surface &s, const char *subtitle)
{
    s.text(kContentX, 48, kCategories[g_active_category].label, theme().text, 1);
    s.text(kContentX, 68, subtitle, theme().text_dim, 1);
}

void draw_cycle_page(Surface &s, const char *subtitle, const char *section_title)
{
    draw_page_title(s, subtitle);
    g_target_count = 0;

    int y = draw_section_header(s, kRowsStartY, section_title, g_section_open[g_active_category], true);
    if (!g_section_open[g_active_category])
        return;

    for (int i = 0; i < kSettingCount; i++) {
        if (g_settings[i].category != g_active_category)
            continue;
        bool hover = (i == g_hover_setting);
        draw_row(s, y, g_settings[i].label, g_settings[i].choices[g_settings[i].current], true, hover);
        register_target(y, kRowH, TARGET_SETTING, i);
        y += kRowH + kRowGap;
    }
}

void draw_display_page(Surface &s)
{
    char resolution[32];
    char depth[24];

    make_resolution_text(resolution, sizeof(resolution));
    make_color_depth_text(depth, sizeof(depth));

    draw_page_title(s, "Live display details from screen_info().");
    g_target_count = 0;

    int y = draw_section_header(s, kRowsStartY, "Display Details", g_section_open[CAT_DISPLAY], true);
    if (!g_section_open[CAT_DISPLAY])
        return;

    draw_row(s, y + 0 * (kRowH + kRowGap), "Resolution", resolution, false, false);
    draw_row(s, y + 1 * (kRowH + kRowGap), "Color Depth", depth, false, false);
    draw_row(s, y + 2 * (kRowH + kRowGap), "Window Server", "MKDX compositor", false, false);
    draw_row(s, y + 3 * (kRowH + kRowGap), "Refresh Rate", "n/a", false, false);
}

void draw_network_page(Surface &s)
{
    draw_page_title(s, "Usermode preferences with live placeholders where kernel data is unavailable.");
    g_target_count = 0;

    int y = draw_section_header(s, kRowsStartY, "Network Preferences", g_section_open[CAT_NETWORK], true);
    if (!g_section_open[CAT_NETWORK])
        return;

    draw_row(s, y, "Link State", "n/a", false, false);
    draw_row(s, y + (kRowH + kRowGap), "IPv4 Mode", g_settings[8].choices[g_settings[8].current], true, g_hover_setting == 8);
    register_target(y + (kRowH + kRowGap), kRowH, TARGET_SETTING, 8);
    draw_row(s, y + 2 * (kRowH + kRowGap), "Hostname", g_settings[9].choices[g_settings[9].current], true, g_hover_setting == 9);
    register_target(y + 2 * (kRowH + kRowGap), kRowH, TARGET_SETTING, 9);
    draw_row(s, y + 3 * (kRowH + kRowGap), "DNS", "n/a", false, false);
}

void draw_storage_page(Surface &s)
{
    draw_page_title(s, "Simple persisted controls plus filesystem placeholders.");
    g_target_count = 0;

    int y = draw_section_header(s, kRowsStartY, "Storage Preferences", g_section_open[CAT_STORAGE], true);
    if (!g_section_open[CAT_STORAGE])
        return;

    draw_row(s, y, "Mounted Root", "/", false, false);
    draw_row(s, y + (kRowH + kRowGap), "Cleanup Schedule", g_settings[12].choices[g_settings[12].current], true, g_hover_setting == 12);
    register_target(y + (kRowH + kRowGap), kRowH, TARGET_SETTING, 12);
    draw_row(s, y + 2 * (kRowH + kRowGap), "Available Space", "n/a", false, false);
}

void draw_about_page(Surface &s)
{
    char resolution[32];
    make_resolution_text(resolution, sizeof(resolution));

    draw_page_title(s, "System Information deep-link lands here.");
    g_target_count = 0;

    int y = draw_section_header(s, kRowsStartY, "About This System", g_section_open[CAT_ABOUT], true);
    if (!g_section_open[CAT_ABOUT])
        return;

    draw_row(s, y + 0 * (kRowH + kRowGap), "Product", "HSRC OS", false, false);
    draw_row(s, y + 1 * (kRowH + kRowGap), "Settings App", "os-settings.mke", false, false);
    draw_row(s, y + 2 * (kRowH + kRowGap), "Display", resolution, false, false);
    draw_row(s, y + 3 * (kRowH + kRowGap), "Status", "usermode control center", false, false);
}

void paint()
{
    if (!g_win.ok())
        return;

    Surface &s = g_win.surface();
    s.clear(theme().bg);
    s.draw_window_chrome(kWinW, g_win_opts.title, g_win_opts, theme().chrome, theme().text, theme().border);
    draw_sidebar(s);
    s.fill(kSidebarW, kChromeTitleH, kWinW - kSidebarW, kHeaderH, theme().panel);
    s.fill(kSidebarW, kChromeTitleH + kHeaderH - 1, kWinW - kSidebarW, 1, theme().border);

    switch (g_active_category) {
    case CAT_GENERAL:
        draw_cycle_page(s, "Click any row to cycle and persist a value.", "General Preferences");
        break;
    case CAT_KEYBOARD:
        draw_cycle_page(s, "Layout and repeat values are stored in /etc/os-settings.ini.", "Keyboard Preferences");
        break;
    case CAT_MOUSE:
        draw_cycle_page(s, "Mouse preferences are persisted as simple key/value pairs.", "Mouse Preferences");
        break;
    case CAT_DISPLAY:
        draw_display_page(s);
        break;
    case CAT_NETWORK:
        draw_network_page(s);
        break;
    case CAT_DESKTOP_DOCK:
        draw_cycle_page(s, "Desktop shell can consume these persisted preferences later.", "Desktop & Dock");
        break;
    case CAT_STORAGE:
        draw_storage_page(s);
        break;
    case CAT_DATE_TIME:
        draw_cycle_page(s, "Clock and timezone choices are usermode preferences for now.", "Date & Time");
        break;
    case CAT_SOUND:
        draw_cycle_page(s, "Audio controls are placeholders until deeper device APIs arrive.", "Sound Preferences");
        break;
    case CAT_ABOUT:
        draw_about_page(s);
        break;
    default:
        draw_cycle_page(s, "", "Preferences");
        break;
    }

    g_win.damage();
    g_dirty = false;
}

bool build_ui()
{
    int screen_w = (int)g_screen.width;
    int screen_h = (int)g_screen.height;

    WindowOptions opts;
    opts.x = screen_w > kWinW ? (screen_w - kWinW) / 2 : 40;
    opts.y = screen_h > kWinH ? (screen_h - kWinH) / 2 : 40;
    opts.w = kWinW;
    opts.h = kWinH;
    opts.background = false;
    opts.rounded = true;
    opts.shadow = true;
    opts.radius = 10;
    opts.resizable = false;
    opts.framed = true;
    opts.closable = true;
    opts.can_minimize = true;
    opts.can_maximize = true;
    opts.accept_focus = true;
    opts.set_title("System Settings");
    opts.set_class_name("os-settings");

    if (!g_win.create(opts))
        return false;
    return refresh_window_options();
}

void update_hover(int lx, int ly)
{
    int next_sidebar = sidebar_hit(lx, ly);
    int next_setting = -1;
    const ClickTarget *hit = target_hit(lx, ly);
    if (hit && hit->kind == TARGET_SETTING)
        next_setting = hit->index;

    if (next_sidebar != g_hover_sidebar || next_setting != g_hover_setting) {
        g_hover_sidebar = next_sidebar;
        g_hover_setting = next_setting;
        g_dirty = true;
    }
}

void handle_click(const Input &in)
{
    if (!refresh_window_options())
        return;

    int lx = in.mouse_x - g_win_opts.x;
    int ly = in.mouse_y - g_win_opts.y;

    if (lx < 0 || ly < 0 || lx >= g_win_opts.w || ly >= g_win_opts.h)
        return;

    ChromeHit chrome = g_win.hit_chrome(lx, ly, g_win_opts);
    if (chrome != ChromeHit::None) {
        (void)g_win.handle_chrome_hit(chrome);
        (void)refresh_window_options();
        g_dirty = true;
        return;
    }

    if (lx >= kContentX) {
        const ClickTarget *hit = target_hit(lx, ly);
        if (hit) {
            if (hit->kind == TARGET_SECTION) {
                int cat = hit->index;
                if (cat >= 0 && cat < CAT_COUNT) {
                    g_section_open[cat] = !g_section_open[cat];
                    g_hover_setting = -1;
                    g_dirty = true;
                }
                return;
            }
            if (hit->kind == TARGET_SETTING) {
                cycle_setting(hit->index);
                return;
            }
        }
    }

    int cat = sidebar_hit(lx, ly);
    if (cat >= 0 && g_active_category != cat) {
        g_active_category = cat;
        g_hover_setting = -1;
        g_dirty = true;
    }
}

} // namespace

extern "C" void mke_main(void)
{
    /* Explicit — do not rely on CRT dynamic/static init for globals. */
    g_running = true;
    g_dirty = true;

    if (!hsrc::sdk::screen_info(g_screen) || g_screen.width == 0 || g_screen.height == 0) {
        for (;;)
            hsrc::sdk::yield();
    }

    for (int i = 0; i < CAT_COUNT; i++)
        g_section_open[i] = true;

    load_settings();
    (void)refresh_theme();

    if (!build_ui()) {
        for (;;)
            hsrc::sdk::yield();
    }

    g_win.show(true);
    g_win.focus();
    poll_deeplink();
    paint();
    (void)hsrc::sdk::present();

    for (;;) {
        if (!g_running || !g_win.ok()) {
            (void)g_win.close();
            hsrc::sdk::exit(0);
        }

        poll_deeplink();

        g_theme_poll++;
        if (g_theme_poll >= kThemePollEvery) {
            g_theme_poll = 0;
            if (refresh_theme())
                g_dirty = true;
        }

        Input in{};
        if (hsrc::sdk::input(in)) {
            if (refresh_window_options()) {
                int lx = in.mouse_x - g_win_opts.x;
                int ly = in.mouse_y - g_win_opts.y;
                if (lx >= 0 && ly >= 0 && lx < g_win_opts.w && ly < g_win_opts.h)
                    update_hover(lx, ly);
            }

            uint8_t pressed = (uint8_t)(in.buttons & ~g_prev_input.buttons);
            if (pressed & UGX_BTN_LEFT) {
                int click_lx = in.mouse_x - g_win_opts.x;
                int click_ly = in.mouse_y - g_win_opts.y;
                bool over = click_lx >= 0 && click_ly >= 0 &&
                            click_lx < g_win_opts.w && click_ly < g_win_opts.h;
                if (over || in.focus_id == g_win.id())
                    handle_click(in);
            }
            g_prev_input = in;
        }

        if (g_dirty)
            paint();
        (void)hsrc::sdk::present();
        hsrc::sdk::yield();
    }
}
