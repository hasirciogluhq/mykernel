#include <user/mke.h>
#include <user/sdk/fs.hpp>
#include <user/sdk/gfx.hpp>
#include <user/sdk/process.hpp>
#include <user/sdk/settings.hpp>
#include <user/sdk/sync.hpp>
#include <user/sdk/thread.hpp>
#include <user/string.h>

/*
 * Files — gfx API explorer, terminal-clean chrome + list.
 */

namespace {

using hsrc::sdk::ChromeHit;
using hsrc::sdk::Color;
using hsrc::sdk::GxDevice;
using hsrc::sdk::Input;
using hsrc::sdk::ScreenInfo;
using hsrc::sdk::Surface;
using hsrc::sdk::Window;
using hsrc::sdk::WindowOptions;
using hsrc::sdk::kGxWaitForever;
using hsrc::sdk::kChromeTitleH;
using hsrc::sdk::kUIFontH;
using hsrc::sdk::ui_panel_body_top;
using hsrc::sdk::ui_panel_text_y;
using hsrc::sdk::ui_text_inset_y;
using hsrc::sdk::settings::theme;
using hsrc::sdk::settings::refresh_theme;

constexpr int kWinW = 720;
constexpr int kWinH = 460;
constexpr int kPad = 12;
constexpr int kRowH = 22;
constexpr int kVisibleRows = 14;
constexpr int kListY = ui_panel_body_top(2);
constexpr int kMaxEntries = 96;
constexpr int kStatusChars = 120;
constexpr int kThemePollEvery = 96;

struct Entry {
    char name[64];
    uint32_t type;
    bool synthetic_up;
};

Window g_win;
GxDevice g_gx;
WindowOptions g_win_opts;
ScreenInfo g_screen{};
Input g_prev_input{};
Entry g_entries[kMaxEntries];
int g_entry_count = 0;
int g_selected = -1;
int g_scroll = 0;
int g_theme_poll = 0;
bool g_dirty = true;
bool g_was_minimized = false;
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
    if (!g_win.get_options(g_win_opts))
        return false;
    if (g_was_minimized && !g_win_opts.minimized && g_win_opts.visible)
        g_dirty = true;
    g_was_minimized = g_win_opts.minimized;
    return true;
}

void join_path(char *out, size_t out_size, const char *base, const char *name)
{
    copy_text(out, out_size, "");
    if (!base || !base[0])
        append_text(out, out_size, "/");
    else
        append_text(out, out_size, base);
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
        set_status("cannot open directory");
        return false;
    }

    /* User stacks are 8KiB; kernel getdents also caps at 32. Avoid 64×dirent (~4.5KiB). */
    constexpr int kDentBatch = 32;
    vfs_dirent_t raw[kDentBatch];
    long count = hsrc::sdk::getdents(fd, raw, kDentBatch);
    (void)hsrc::sdk::close(fd);
    if (count < 0) {
        set_status("listing failed");
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
    set_status("click select · click again open");
    return true;
}

bool navigate_to(const char *path)
{
    if (!path || !path[0]) {
        set_status("invalid path");
        return false;
    }
    if (hsrc::sdk::chdir(path) < 0) {
        set_status("cd failed");
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
        return "dir";
    if (ends_with(entry.name, ".mke"))
        return "app";
    return "file";
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
        set_status("only folders and .mke apps");
        g_dirty = true;
        return false;
    }

    char full[VFS_PATH_MAX];
    join_path(full, sizeof(full), g_cwd, entry.name);
    long pid = hsrc::sdk::process::spawn_ex(full, hsrc::sdk::process::ConsoleHidden);
    if (pid <= 0) {
        set_status("launch failed");
        g_dirty = true;
        return false;
    }

    char status[kStatusChars];
    copy_text(status, sizeof(status), "launched ");
    append_text(status, sizeof(status), full);
    set_status(status);
    g_dirty = true;
    return true;
}

void paint()
{
    const auto &t = theme();
    Surface &s = g_win.surface();
    s.fill(0, kChromeTitleH, kWinW, kWinH - kChromeTitleH, t.bg);

    s.text(kPad, ui_panel_text_y(0), "Files", t.text, 1);
    s.text(kPad + 56, ui_panel_text_y(0), g_cwd, t.accent, 1);

    /* Quick jumps */
    s.text(kPad, ui_panel_text_y(1), "[apps]", t.text_dim, 1);
    s.text(kPad + 56, ui_panel_text_y(1), "[/]", t.text_dim, 1);
    s.text(kPad + 96, ui_panel_text_y(1), "[..]", t.text_dim, 1);
    s.text(kPad + 140, ui_panel_text_y(1), "name", t.text_dim, 1);
    s.text(kWinW - kPad - 40, ui_panel_text_y(1), "type", t.text_dim, 1);
    s.fill(kPad, ui_panel_text_y(1) + kUIFontH + 4, kWinW - kPad * 2, 1, t.border);

    const int row_text_dy = ui_text_inset_y(kRowH);
    for (int row = 0; row < kVisibleRows; row++) {
        const int index = g_scroll + row;
        const int y = kListY + row * kRowH;
        if (index >= g_entry_count)
            continue;

        const Entry &entry = g_entries[index];
        const bool selected = index == g_selected;
        if (selected)
            s.fill(kPad, y, kWinW - kPad * 2, kRowH - 2, t.accent_soft);

        s.text(kPad + 4, y + row_text_dy, entry.name, entry_name_color(entry), 1);
        const char *type = entry_type_label(entry);
        s.text(kWinW - kPad - text_width(type), y + row_text_dy, type,
               selected ? t.accent : t.text_soft, 1);
    }

    s.fill(kPad, kWinH - 28, kWinW - kPad * 2, 1, t.border);
    s.text(kPad, kWinH - 20, g_status, t.text_dim, 1);

    const bool can_prev = g_scroll > 0;
    const bool can_next = g_scroll + kVisibleRows < g_entry_count;
    s.text(kWinW - 120, kWinH - 20, can_prev ? "prev" : "    ", t.text_dim, 1);
    s.text(kWinW - 60, kWinH - 20, can_next ? "next" : "    ", t.text_dim, 1);

    g_dirty = false;
}

int jump_hit(int lx, int ly)
{
    const int row_y = ui_panel_text_y(1);
    if (ly < row_y || ly >= row_y + kUIFontH)
        return -1;
    if (lx >= kPad && lx < kPad + 50)
        return 0; /* apps */
    if (lx >= kPad + 56 && lx < kPad + 90)
        return 1; /* / */
    if (lx >= kPad + 96 && lx < kPad + 130)
        return 2; /* .. */
    return -1;
}

int row_hit(int lx, int ly)
{
    if (lx < kPad || lx >= kWinW - kPad)
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
    if (ly < kWinH - 24 || ly >= kWinH - 6)
        return -1;
    if (lx >= kWinW - 120 && lx < kWinW - 70)
        return 0;
    if (lx >= kWinW - 60 && lx < kWinW - 20)
        return 1;
    return -1;
}

void handle_click(const Input &in)
{
    if (!refresh_window_options())
        return;
    if (g_win_opts.minimized || !g_win_opts.visible)
        return;

    const int lx = in.mouse_x - g_win_opts.x;
    const int ly = in.mouse_y - g_win_opts.y;
    if (lx < 0 || ly < 0 || lx >= g_win_opts.w || ly >= g_win_opts.h)
        return;

    /* Chrome buttons: GxDevice::wait */

    const int jump = jump_hit(lx, ly);
    if (jump == 0) {
        (void)navigate_to("/applications");
        return;
    }
    if (jump == 1) {
        (void)navigate_to("/");
        return;
    }
    if (jump == 2) {
        if (!is_root())
            (void)navigate_to("..");
        else {
            set_status("already at root");
            g_dirty = true;
        }
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
    set_status("selected · click again to open");
    g_dirty = true;
}

} // namespace

extern "C" void mke_main(void)
{
    if (!hsrc::sdk::screen_info(g_screen) || g_screen.width == 0 || g_screen.height == 0) {
        for (;;)
            hsrc::sdk::this_thread::sleep_for(1000u);
    }

    (void)refresh_theme();

    WindowOptions opts;
    opts.x = g_screen.width > (uint32_t)kWinW ? ((int)g_screen.width - kWinW) / 2 : 30;
    opts.y = g_screen.height > (uint32_t)kWinH ? ((int)g_screen.height - kWinH) / 2 : 30;
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
    opts.set_title("Files");
    opts.set_class_name("os.files");

    if (!g_win.create(opts))
        hsrc::sdk::exit(1);
    if (!g_gx.create(g_win))
        hsrc::sdk::exit(1);
    (void)refresh_window_options();

    /* Show first so a slow/failed listing still leaves a visible window. */
    g_win.show(true);
    g_win.focus();
    if (!navigate_to("/applications"))
        (void)navigate_to("/");
    g_gx.set_chrome_colors(theme().chrome, theme().text, theme().border);

    for (;;) {
        if (!g_win.ok()) {
            (void)g_win.close();
            hsrc::sdk::exit(0);
        }

        g_theme_poll++;
        if (g_theme_poll >= kThemePollEvery) {
            g_theme_poll = 0;
            if (refresh_theme()) {
                g_gx.set_chrome_colors(theme().chrome, theme().text, theme().border);
                g_dirty = true;
            }
        }

        (void)refresh_window_options();

        const uint32_t wait_to =
            g_win_opts.minimized ? 200u : kGxWaitForever;
        Input in = g_gx.wait(wait_to);
        if (g_gx.dragging())
            continue;

        {
            const uint8_t pressed = (uint8_t)(in.buttons & ~g_prev_input.buttons);
            if (pressed & UGX_BTN_LEFT) {
                const bool interactive = !g_win_opts.minimized && g_win_opts.visible;
                if (interactive && in.hit_id == g_win.id())
                    handle_click(in);
            }
            g_prev_input = in;
        }

        if (!g_win_opts.minimized && g_dirty) {
            (void)g_gx.begin_scene();
            paint();
            (void)g_gx.end_scene();
            (void)g_gx.present();
        }
    }
}
