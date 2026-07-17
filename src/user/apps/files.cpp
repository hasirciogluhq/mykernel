#include <user/mke.h>
#include <user/sdk/fs.hpp>
#include <user/sdk/gfx.hpp>
#include <user/sdk/process.hpp>
#include <user/sdk/settings.hpp>
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

constexpr int kWinW = 760;
constexpr int kWinH = 548;
constexpr int kSidebarW = 172;
constexpr int kHeaderH = 58;
constexpr int kFooterH = 34;
constexpr int kListX = kSidebarW + 18;
constexpr int kListW = kWinW - kListX - 18;
constexpr int kListY = kChromeTitleH + 92;
constexpr int kRowH = 32;
constexpr int kVisibleRows = 12;
constexpr int kShortcutX = 14;
constexpr int kShortcutW = kSidebarW - 28;
constexpr int kShortcutH = 30;
constexpr int kShortcutGap = 8;
constexpr int kShortcutTop = kChromeTitleH + 88;
constexpr int kFooterBtnW = 72;
constexpr int kMaxEntries = 96;
constexpr int kStatusChars = 120;
constexpr int kThemePollEvery = 96;

struct Entry {
    char name[64];
    uint32_t type;
    bool synthetic_up;
};

Window g_win;
WindowOptions g_win_opts;
ScreenInfo g_screen{};
Input g_prev_input{};
Entry g_entries[kMaxEntries];
int g_entry_count = 0;
int g_selected = -1;
int g_scroll = 0;
int g_theme_poll = 0;
bool g_dirty = true;
char g_cwd[VFS_PATH_MAX];
char g_status[kStatusChars];

void copy_text(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0)
        return;
    dst[0] = 0;
    if (!src)
        return;
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = 0;
}

void append_text(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0 || !src)
        return;
    size_t len = strlen(dst);
    if (len >= dst_size - 1)
        return;
    strncpy(dst + len, src, dst_size - len - 1);
    dst[dst_size - 1] = 0;
}

int text_width(const char *s)
{
    return Surface::text_width(s, 1);
}

bool ends_with(const char *text, const char *suffix)
{
    size_t text_len = strlen(text ? text : "");
    size_t suffix_len = strlen(suffix ? suffix : "");
    if (suffix_len > text_len)
        return false;
    return strcmp(text + text_len - suffix_len, suffix) == 0;
}

bool is_root()
{
    return strcmp(g_cwd, "/") == 0;
}

void set_status(const char *text)
{
    copy_text(g_status, sizeof(g_status), text);
}

bool refresh_window_options()
{
    return g_win.get_options(g_win_opts);
}

void join_path(char *out, size_t out_size, const char *base, const char *name)
{
    copy_text(out, out_size, "");
    if (!base || !base[0]) {
        append_text(out, out_size, "/");
    } else {
        append_text(out, out_size, base);
    }
    if (strcmp(out, "/") != 0)
        append_text(out, out_size, "/");
    append_text(out, out_size, name);
}

void refresh_cwd()
{
    if (hsrc::sdk::getcwd(g_cwd, sizeof(g_cwd)) < 0)
        copy_text(g_cwd, sizeof(g_cwd), "/");
}

bool load_entries()
{
    int fd = (int)hsrc::sdk::open(".", O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        set_status("Current directory cannot be opened.");
        return false;
    }

    vfs_dirent_t raw[64];
    long count = hsrc::sdk::getdents(fd, raw, 64);
    (void)hsrc::sdk::close(fd);
    if (count < 0) {
        set_status("Directory listing failed.");
        return false;
    }

    g_entry_count = 0;
    if (!is_root()) {
        Entry &up = g_entries[g_entry_count++];
        copy_text(up.name, sizeof(up.name), "..");
        up.type = S_IFDIR;
        up.synthetic_up = true;
    }

    for (long i = 0; i < count && g_entry_count < kMaxEntries; i++) {
        if (strcmp(raw[i].name, ".") == 0 || strcmp(raw[i].name, "..") == 0)
            continue;
        Entry &entry = g_entries[g_entry_count++];
        copy_text(entry.name, sizeof(entry.name), raw[i].name);
        entry.type = raw[i].type;
        entry.synthetic_up = false;
    }

    g_selected = -1;
    g_scroll = 0;
    set_status("Click once to select, click again to open.");
    return true;
}

bool navigate_to(const char *path)
{
    if (!path || !path[0]) {
        set_status("Invalid path.");
        return false;
    }
    if (hsrc::sdk::chdir(path) < 0) {
        set_status("Navigation failed.");
        return false;
    }
    refresh_cwd();
    if (!load_entries())
        return false;
    g_dirty = true;
    return true;
}

const char *entry_type_label(const Entry &entry)
{
    if (entry.synthetic_up || S_ISDIR(entry.type))
        return "Dir";
    if (ends_with(entry.name, ".mke"))
        return "App";
    return "File";
}

Color entry_name_color(const Entry &entry)
{
    const auto &t = theme();
    if (entry.synthetic_up || S_ISDIR(entry.type))
        return t.accent;
    if (ends_with(entry.name, ".mke"))
        return t.warn;
    return t.text;
}

bool activate_entry(int index)
{
    if (index < 0 || index >= g_entry_count)
        return false;

    const Entry &entry = g_entries[index];
    if (entry.synthetic_up || S_ISDIR(entry.type))
        return navigate_to(entry.synthetic_up ? ".." : entry.name);

    if (!ends_with(entry.name, ".mke")) {
        set_status("Only folders and .mke apps are activatable.");
        g_dirty = true;
        return false;
    }

    char full[VFS_PATH_MAX];
    join_path(full, sizeof(full), g_cwd, entry.name);
    long pid = hsrc::sdk::process::spawn_ex(full, hsrc::sdk::process::ConsoleHidden);
    if (pid <= 0) {
        set_status("App launch failed.");
        g_dirty = true;
        return false;
    }

    char status[kStatusChars];
    copy_text(status, sizeof(status), "Launched ");
    append_text(status, sizeof(status), full);
    set_status(status);
    g_dirty = true;
    return true;
}

void paint_shortcut(Surface &s, int y, const char *label, bool active)
{
    const auto &t = theme();
    s.fill_round(kShortcutX, y, kShortcutW, kShortcutH, 8, active ? t.accent_soft : t.button);
    s.rect(kShortcutX, y, kShortcutW, kShortcutH, active ? t.accent_soft : t.border, 1);
    s.text(kShortcutX + 12, y + 10, label, active ? t.accent : t.text, 1);
}

void paint()
{
    const auto &t = theme();
    Surface &s = g_win.surface();
    s.clear(t.bg);
    s.draw_window_chrome(kWinW, g_win_opts.title, g_win_opts, t.chrome, t.text, t.border);

    s.fill(0, kChromeTitleH, kSidebarW, kWinH - kChromeTitleH, t.sidebar);
    s.fill(kSidebarW - 1, kChromeTitleH, 1, kWinH - kChromeTitleH, t.border);
    s.fill(kSidebarW, kChromeTitleH + kHeaderH - 1, kWinW - kSidebarW, 1, t.border);
    s.fill(0, kWinH - kFooterH, kWinW, 1, t.border);

    s.text(16, kChromeTitleH + 14, "Files", t.text, 1);
    s.text(16, kChromeTitleH + 36, "Bounded explorer", t.text_dim, 1);

    int shortcut_y = kShortcutTop;
    paint_shortcut(s, shortcut_y, "Applications", strcmp(g_cwd, "/applications") == 0);
    shortcut_y += kShortcutH + kShortcutGap;
    paint_shortcut(s, shortcut_y, "Root", is_root());
    shortcut_y += kShortcutH + kShortcutGap;
    paint_shortcut(s, shortcut_y, "Up", false);

    s.text(kListX, kChromeTitleH + 18, "Current Path", t.text_dim, 1);
    s.fill_round(kListX, kChromeTitleH + 34, kListW, 30, 8, t.inset);
    s.rect(kListX, kChromeTitleH + 34, kListW, 30, t.border, 1);
    s.text(kListX + 12, kChromeTitleH + 44, g_cwd, t.text, 1);

    s.text(kListX, kChromeTitleH + 74, "Name", t.text_dim, 1);
    s.text(kListX + kListW - 72, kChromeTitleH + 74, "Type", t.text_dim, 1);

    for (int row = 0; row < kVisibleRows; row++) {
        const int index = g_scroll + row;
        const int y = kListY + row * kRowH;
        if (index >= g_entry_count) {
            s.fill_round(kListX, y, kListW, kRowH - 4, 8, t.card);
            s.rect(kListX, y, kListW, kRowH - 4, t.border, 1);
            continue;
        }

        const Entry &entry = g_entries[index];
        const bool selected = index == g_selected;
        s.fill_round(kListX, y, kListW, kRowH - 4, 8, selected ? t.accent_soft : t.hover);
        s.rect(kListX, y, kListW, kRowH - 4, t.border, 1);
        s.text(kListX + 12, y + 9, entry.name, entry_name_color(entry), 1);
        const char *type = entry_type_label(entry);
        s.text(kListX + kListW - 12 - text_width(type), y + 9, type,
               selected ? t.accent : t.text_soft, 1);
    }

    s.text(16, kWinH - 22, g_status, t.text_dim, 1);

    const bool can_prev = g_scroll > 0;
    const bool can_next = g_scroll + kVisibleRows < g_entry_count;
    const int next_x = kWinW - 16 - kFooterBtnW;
    const int prev_x = next_x - 10 - kFooterBtnW;
    s.fill_round(prev_x, kWinH - 28, kFooterBtnW, 22, 6, can_prev ? t.button : t.inset);
    s.fill_round(next_x, kWinH - 28, kFooterBtnW, 22, 6, can_next ? t.button : t.inset);
    s.rect(prev_x, kWinH - 28, kFooterBtnW, 22, t.border, 1);
    s.rect(next_x, kWinH - 28, kFooterBtnW, 22, t.border, 1);
    s.text(prev_x + 19, kWinH - 20, "Prev", can_prev ? t.text : t.text_soft, 1);
    s.text(next_x + 19, kWinH - 20, "Next", can_next ? t.text : t.text_soft, 1);

    g_win.damage();
    g_dirty = false;
}

int shortcut_hit(int lx, int ly)
{
    int shortcut_y = kShortcutTop;
    for (int i = 0; i < 3; i++) {
        if (lx >= kShortcutX && lx < kShortcutX + kShortcutW &&
            ly >= shortcut_y && ly < shortcut_y + kShortcutH)
            return i;
        shortcut_y += kShortcutH + kShortcutGap;
    }
    return -1;
}

int row_hit(int lx, int ly)
{
    if (lx < kListX || lx >= kListX + kListW)
        return -1;
    if (ly < kListY || ly >= kListY + kVisibleRows * kRowH)
        return -1;
    int row = (ly - kListY) / kRowH;
    int index = g_scroll + row;
    if (index < 0 || index >= g_entry_count)
        return -1;
    return index;
}

int footer_hit(int lx, int ly)
{
    const int next_x = kWinW - 16 - kFooterBtnW;
    const int prev_x = next_x - 10 - kFooterBtnW;
    if (ly < kWinH - 28 || ly >= kWinH - 6)
        return -1;
    if (lx >= prev_x && lx < prev_x + kFooterBtnW)
        return 0;
    if (lx >= next_x && lx < next_x + kFooterBtnW)
        return 1;
    return -1;
}

void handle_click(const Input &in)
{
    if (!refresh_window_options())
        return;

    const int lx = in.mouse_x - g_win_opts.x;
    const int ly = in.mouse_y - g_win_opts.y;
    if (lx < 0 || ly < 0 || lx >= g_win_opts.w || ly >= g_win_opts.h)
        return;

    ChromeHit chrome = g_win.hit_chrome(lx, ly, g_win_opts);
    if (chrome != ChromeHit::None) {
        (void)g_win.handle_chrome_hit(chrome);
        (void)refresh_window_options();
        g_dirty = true;
        return;
    }

    const int shortcut = shortcut_hit(lx, ly);
    if (shortcut == 0) {
        (void)navigate_to("/applications");
        return;
    }
    if (shortcut == 1) {
        (void)navigate_to("/");
        return;
    }
    if (shortcut == 2) {
        if (!is_root())
            (void)navigate_to("..");
        else
            set_status("Already at root.");
        g_dirty = true;
        return;
    }

    const int footer = footer_hit(lx, ly);
    if (footer == 0 && g_scroll > 0) {
        g_scroll -= kVisibleRows;
        if (g_scroll < 0)
            g_scroll = 0;
        g_dirty = true;
        return;
    }
    if (footer == 1 && g_scroll + kVisibleRows < g_entry_count) {
        g_scroll += kVisibleRows;
        if (g_scroll >= g_entry_count)
            g_scroll = g_entry_count - 1;
        g_dirty = true;
        return;
    }

    const int entry = row_hit(lx, ly);
    if (entry < 0)
        return;

    if (g_selected == entry) {
        (void)activate_entry(entry);
        return;
    }

    g_selected = entry;
    set_status("Selected. Click again to open.");
    g_dirty = true;
}

bool build_ui()
{
    WindowOptions opts;
    opts.x = g_screen.width > (uint32_t)kWinW ? ((int)g_screen.width - kWinW) / 2 : 30;
    opts.y = g_screen.height > (uint32_t)kWinH ? ((int)g_screen.height - kWinH) / 2 : 30;
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
    opts.set_title("Files");
    opts.set_class_name("os.files");

    if (!g_win.create(opts))
        return false;
    return refresh_window_options();
}

} // namespace

extern "C" void mke_main(void)
{
    if (!hsrc::sdk::screen_info(g_screen) || g_screen.width == 0 || g_screen.height == 0) {
        for (;;)
            hsrc::sdk::yield();
    }

    (void)refresh_theme();

    if (!build_ui()) {
        for (;;)
            hsrc::sdk::yield();
    }

    if (!navigate_to("/applications"))
        (void)navigate_to("/");

    g_win.show(true);
    g_win.focus();
    paint();
    (void)hsrc::sdk::present();

    for (;;) {
        if (!g_win.ok()) {
            (void)g_win.close();
            hsrc::sdk::exit(0);
        }

        g_theme_poll++;
        if (g_theme_poll >= kThemePollEvery) {
            g_theme_poll = 0;
            if (refresh_theme())
                g_dirty = true;
        }

        Input in{};
        if (hsrc::sdk::input(in)) {
            const uint8_t pressed = (uint8_t)(in.buttons & ~g_prev_input.buttons);
            if (pressed & UGX_BTN_LEFT) {
                const int lx = in.mouse_x - g_win_opts.x;
                const int ly = in.mouse_y - g_win_opts.y;
                const bool over = refresh_window_options() &&
                                  lx >= 0 && ly >= 0 &&
                                  lx < g_win_opts.w && ly < g_win_opts.h;
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
