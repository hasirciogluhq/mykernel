#include <user/mke.h>
#include <user/sdk/gfx.hpp>
#include <user/sdk/process.hpp>
#include <user/sdk/settings.hpp>
#include <user/string.h>

/*
 * Activity Monitor — reads shared proc_page (no PROC_LIST/SYSINFO spam).
 */

namespace {

using hsrc::sdk::ChromeHit;
using hsrc::sdk::Color;
using hsrc::sdk::Input;
using hsrc::sdk::ScreenInfo;
using hsrc::sdk::Surface;
using hsrc::sdk::Window;
using hsrc::sdk::WindowOptions;
using hsrc::sdk::kChromeTitleH;
using hsrc::sdk::kUIFontH;
using hsrc::sdk::ui_panel_body_top;
using hsrc::sdk::ui_panel_text_y;
using hsrc::sdk::ui_text_inset_y;
using hsrc::sdk::process::ProcListEntry;
using hsrc::sdk::process::ProcStat;
using hsrc::sdk::process::SysInfo;
using hsrc::sdk::settings::theme;
using hsrc::sdk::settings::refresh_theme;

constexpr int kWinW = 780;
constexpr int kWinH = 480;
constexpr int kPad = 12;
constexpr int kRowH = 22;
constexpr int kVisibleRows = 12;
constexpr int kHeaderLines = 3;
constexpr int kListY = ui_panel_body_top(kHeaderLines);
/* User stacks are 8KiB — keep snapshot buffers in BSS, not on the stack. */
constexpr int kMaxEntries = 96;
constexpr int kStatusChars = 128;
constexpr int kThemePollEvery = 240;
/* ~1–2 Hz sample cadence (yield-counted, not per-yield publish). */
constexpr int kSampleEvery = 24;

struct MonitorEntry {
    ProcListEntry proc{};
    uint32_t cpu_pct = 0;
};

struct PrevSample {
    pid_t pid = 0;
    uint64_t cpu_ticks = 0;
};

Window g_win;
WindowOptions g_win_opts;
ScreenInfo g_screen{};
Input g_prev_input{};
MonitorEntry g_entries[kMaxEntries];
PrevSample g_prev_samples[kMaxEntries];
ProcListEntry g_snapshot_scratch[kMaxEntries];
SysInfo g_sysinfo{};
ProcStat g_selected_stat{};
int g_entry_count = 0;
int g_scroll = 0;
bool g_has_selected_stat = false;
bool g_dirty = true;
bool g_was_minimized = false;
int g_theme_poll = 0;
int g_sample_poll = 0;
pid_t g_selected_pid = -1;
pid_t g_self_pid = -1;
uint64_t g_prev_total_ticks = 0;
uint32_t g_total_cpu_pct = 0;
uint64_t g_last_sample_ticks = 0;
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

void append_uint(char *dst, size_t dst_size, uint32_t value)
{
    char tmp[16];
    int i = 0;
    if (value == 0) {
        append_text(dst, dst_size, "0");
        return;
    }
    while (value > 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (i-- > 0) {
        char ch[2] = { tmp[i], 0 };
        append_text(dst, dst_size, ch);
    }
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

uint64_t prev_ticks_for(pid_t pid)
{
    for (int i = 0; i < kMaxEntries; i++) {
        if (g_prev_samples[i].pid == pid)
            return g_prev_samples[i].cpu_ticks;
    }
    return 0;
}

uint32_t narrow_u64(uint64_t value)
{
    return value > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)value;
}

void remember_samples()
{
    for (int i = 0; i < kMaxEntries; i++) {
        g_prev_samples[i].pid = 0;
        g_prev_samples[i].cpu_ticks = 0;
    }
    for (int i = 0; i < g_entry_count && i < kMaxEntries; i++) {
        g_prev_samples[i].pid = g_entries[i].proc.pid;
        g_prev_samples[i].cpu_ticks = g_entries[i].proc.cpu_ticks;
    }
}

int selected_index()
{
    for (int i = 0; i < g_entry_count; i++) {
        if (g_entries[i].proc.pid == g_selected_pid)
            return i;
    }
    return -1;
}

bool can_end_task()
{
    int idx = selected_index();
    if (idx < 0)
        return false;
    const ProcListEntry &proc = g_entries[idx].proc;
    return proc.is_user != 0 && proc.pid != g_self_pid &&
           proc.state != hsrc::sdk::process::Zombie;
}

void refresh_monitor(bool keep_status)
{
    SysInfo info{};
    int count = 0;
    int live = 0;

    if (!hsrc::sdk::process::refresh_snapshot()) {
        set_status("proc snapshot unavailable");
        g_dirty = true;
        return;
    }
    if (!hsrc::sdk::process::snapshot(g_snapshot_scratch, kMaxEntries, &count, &info)) {
        set_status("proc snapshot unavailable");
        g_dirty = true;
        return;
    }
    if (count > kMaxEntries)
        count = kMaxEntries;

    /* Drop zombies client-side too (kernel already filters). */
    for (int i = 0; i < count && live < kMaxEntries; i++) {
        if (g_snapshot_scratch[i].state == hsrc::sdk::process::Zombie ||
            g_snapshot_scratch[i].state == hsrc::sdk::process::Unused)
            continue;
        g_entries[live].proc = g_snapshot_scratch[i];
        g_entries[live].cpu_pct = 0;
        live++;
    }
    count = live;

    uint64_t total_delta = 0;
    if (info.total_cpu_ticks >= g_prev_total_ticks)
        total_delta = info.total_cpu_ticks - g_prev_total_ticks;
    uint32_t total_delta32 = narrow_u64(total_delta);
    uint32_t next_cpu = 0;
    if (total_delta32 > 0 && g_last_sample_ticks > 0) {
        uint64_t sample_dt = info.uptime_ticks >= g_last_sample_ticks
                                 ? info.uptime_ticks - g_last_sample_ticks
                                 : 0;
        if (sample_dt == 0)
            sample_dt = 1;
        next_cpu = (total_delta32 * 100u) / narrow_u64(sample_dt);
        if (next_cpu > 100)
            next_cpu = 100;
    }

    for (int i = 0; i < count; i++) {
        if (total_delta32 > 0 && next_cpu > 0) {
            uint64_t prev = prev_ticks_for(g_entries[i].proc.pid);
            uint64_t delta = g_entries[i].proc.cpu_ticks >= prev
                                 ? g_entries[i].proc.cpu_ticks - prev
                                 : 0;
            uint32_t delta32 = narrow_u64(delta);
            g_entries[i].cpu_pct = (delta32 * next_cpu) / total_delta32;
        }
    }

    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            bool swap = false;
            if (g_entries[j].cpu_pct > g_entries[i].cpu_pct)
                swap = true;
            else if (g_entries[j].cpu_pct == g_entries[i].cpu_pct &&
                     g_entries[j].proc.mem_bytes > g_entries[i].proc.mem_bytes)
                swap = true;
            else if (g_entries[j].cpu_pct == g_entries[i].cpu_pct &&
                     g_entries[j].proc.mem_bytes == g_entries[i].proc.mem_bytes &&
                     g_entries[j].proc.pid < g_entries[i].proc.pid)
                swap = true;
            if (swap) {
                MonitorEntry tmp = g_entries[i];
                g_entries[i] = g_entries[j];
                g_entries[j] = tmp;
            }
        }
    }

    g_sysinfo = info;
    g_entry_count = count;
    g_total_cpu_pct = next_cpu;

    remember_samples();
    g_prev_total_ticks = info.total_cpu_ticks;
    g_last_sample_ticks = info.uptime_ticks;

    if (g_selected_pid > 0) {
        int idx = selected_index();
        if (idx >= 0) {
            const ProcListEntry &e = g_entries[idx].proc;
            g_selected_stat.pid = e.pid;
            g_selected_stat.ppid = e.ppid;
            g_selected_stat.state = e.state;
            g_selected_stat.is_user = e.is_user;
            g_selected_stat.cpu_ticks = e.cpu_ticks;
            g_selected_stat.uptime_ticks = e.uptime_ticks;
            g_selected_stat.mem_bytes = e.mem_bytes;
            g_selected_stat.start_ticks = 0;
            copy_text(g_selected_stat.name, sizeof(g_selected_stat.name), e.name);
            g_has_selected_stat = true;
        } else {
            g_has_selected_stat = false;
            g_selected_pid = -1;
        }
    } else {
        g_has_selected_stat = false;
    }

    if (g_scroll > 0 && g_scroll >= g_entry_count)
        g_scroll = g_entry_count > 0 ? g_entry_count - 1 : 0;
    if (!keep_status)
        set_status("select a process · end to terminate");
    /* Always repaint after a successful sample — cpu/state ticks change even
     * when sorted row order looks identical. */
    g_dirty = true;
}

void paint()
{
    if (!g_win.ok())
        return;

    const auto &t = theme();
    Surface &s = g_win.surface();
    s.clear(t.bg);
    s.draw_window_chrome(kWinW, g_win_opts.title, g_win_opts, t.chrome, t.text, t.border);

    s.text(kPad, ui_panel_text_y(0), "Activity Monitor", t.text, 1);

    char line[160];
    line[0] = 0;
    append_text(line, sizeof(line), "cpu ");
    append_uint(line, sizeof(line), g_total_cpu_pct);
    append_text(line, sizeof(line), "%  ram ");
    append_uint(line, sizeof(line), g_sysinfo.used_ram_bytes / 1024u);
    append_text(line, sizeof(line), "/");
    append_uint(line, sizeof(line), g_sysinfo.total_ram_bytes / 1024u);
    append_text(line, sizeof(line), " KB  procs ");
    append_uint(line, sizeof(line), g_sysinfo.process_count);
    s.text(kPad, ui_panel_text_y(1), line, t.text_dim, 1);

    const bool can_kill = can_end_task();
    s.text(kWinW - kPad - 72, ui_panel_text_y(0), "[end]",
           can_kill ? t.danger : t.text_soft, 1);

    s.text(kPad, ui_panel_text_y(2), "name", t.text_dim, 1);
    s.text(kPad + 220, ui_panel_text_y(2), "pid", t.text_dim, 1);
    s.text(kPad + 280, ui_panel_text_y(2), "state", t.text_dim, 1);
    s.text(kPad + 400, ui_panel_text_y(2), "cpu", t.text_dim, 1);
    s.text(kPad + 480, ui_panel_text_y(2), "mem", t.text_dim, 1);
    s.fill(kPad, ui_panel_text_y(2) + kUIFontH + 4, kWinW - kPad * 2, 1, t.border);

    const int row_text_dy = ui_text_inset_y(kRowH);
    for (int row = 0; row < kVisibleRows; row++) {
        const int index = g_scroll + row;
        const int y = kListY + row * kRowH;
        if (index >= g_entry_count)
            continue;

        const MonitorEntry &entry = g_entries[index];
        const bool selected = entry.proc.pid == g_selected_pid;
        if (selected)
            s.fill(kPad, y, kWinW - kPad * 2, kRowH - 2, t.accent_soft);

        char pid_text[16];
        char cpu_text[16];
        char mem_text[24];
        pid_text[0] = 0;
        append_uint(pid_text, sizeof(pid_text), (uint32_t)entry.proc.pid);
        cpu_text[0] = 0;
        append_uint(cpu_text, sizeof(cpu_text), entry.cpu_pct);
        append_text(cpu_text, sizeof(cpu_text), "%");
        mem_text[0] = 0;
        append_uint(mem_text, sizeof(mem_text), entry.proc.mem_bytes / 1024u);
        append_text(mem_text, sizeof(mem_text), "K");

        Color fg = selected ? t.accent : t.text;
        Color dim = selected ? t.accent : t.text_dim;
        s.text(kPad + 4, y + row_text_dy, entry.proc.name, fg, 1);
        s.text(kPad + 220, y + row_text_dy, pid_text, fg, 1);
        s.text(kPad + 280, y + row_text_dy, hsrc::sdk::process::state_name(entry.proc.state), dim, 1);
        s.text(kPad + 400, y + row_text_dy, cpu_text, fg, 1);
        s.text(kPad + 480, y + row_text_dy, mem_text, fg, 1);
    }

    s.fill(kPad, kWinH - 48, kWinW - kPad * 2, 1, t.border);
    if (g_has_selected_stat) {
        char detail[200];
        detail[0] = 0;
        append_text(detail, sizeof(detail), g_selected_stat.name);
        append_text(detail, sizeof(detail), "  pid=");
        append_uint(detail, sizeof(detail), (uint32_t)g_selected_stat.pid);
        append_text(detail, sizeof(detail), "  ");
        append_text(detail, sizeof(detail), hsrc::sdk::process::state_name(g_selected_stat.state));
        s.text(kPad, kWinH - 40, detail, t.text, 1);
    } else {
        s.text(kPad, kWinH - 40, "select a process", t.text_dim, 1);
    }
    s.text(kPad, kWinH - 20, g_status, t.text_dim, 1);

    const bool can_prev = g_scroll > 0;
    const bool can_next = g_scroll + kVisibleRows < g_entry_count;
    s.text(kWinW - 120, kWinH - 20, can_prev ? "prev" : "    ", t.text_dim, 1);
    s.text(kWinW - 60, kWinH - 20, can_next ? "next" : "    ", t.text_dim, 1);

    g_win.damage();
    g_dirty = false;
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

bool end_hit(int lx, int ly)
{
    const int row_y = ui_panel_text_y(0);
    return lx >= kWinW - kPad - 72 && lx < kWinW - kPad &&
           ly >= row_y && ly < row_y + kUIFontH;
}

void perform_end_task()
{
    int idx = selected_index();
    if (idx < 0) {
        set_status("select a process first");
        g_dirty = true;
        return;
    }
    if (!can_end_task()) {
        set_status("cannot end this process");
        g_dirty = true;
        return;
    }

    long rc = hsrc::sdk::process::kill(g_entries[idx].proc.pid);
    if (rc < 0) {
        set_status("end failed");
        g_dirty = true;
        return;
    }

    set_status("terminated");
    refresh_monitor(true);
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

    ChromeHit chrome = g_win.hit_chrome(lx, ly, g_win_opts);
    if (chrome != ChromeHit::None) {
        (void)g_win.handle_chrome_hit(chrome);
        (void)refresh_window_options();
        g_dirty = true;
        return;
    }

    if (end_hit(lx, ly)) {
        perform_end_task();
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

    const int row = row_hit(lx, ly);
    if (row >= 0) {
        g_selected_pid = g_entries[row].proc.pid;
        const ProcListEntry &e = g_entries[row].proc;
        g_selected_stat.pid = e.pid;
        g_selected_stat.ppid = e.ppid;
        g_selected_stat.state = e.state;
        g_selected_stat.is_user = e.is_user;
        g_selected_stat.cpu_ticks = e.cpu_ticks;
        g_selected_stat.uptime_ticks = e.uptime_ticks;
        g_selected_stat.mem_bytes = e.mem_bytes;
        g_selected_stat.start_ticks = 0;
        copy_text(g_selected_stat.name, sizeof(g_selected_stat.name), e.name);
        g_has_selected_stat = true;
        g_dirty = true;
    }
}

} // namespace

extern "C" void mke_main(void)
{
    g_dirty = true;

    if (!hsrc::sdk::screen_info(g_screen) || g_screen.width == 0 || g_screen.height == 0) {
        for (;;)
            hsrc::sdk::yield();
    }

    g_self_pid = (pid_t)hsrc::sdk::process::getpid();
    set_status("collecting...");
    (void)refresh_theme();
    (void)hsrc::sdk::process::map_proc_page();

    WindowOptions opts;
    opts.x = g_screen.width > (uint32_t)kWinW ? ((int)g_screen.width - kWinW) / 2 : 24;
    opts.y = g_screen.height > (uint32_t)kWinH ? ((int)g_screen.height - kWinH) / 2 : 24;
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
    opts.set_title("Activity Monitor");
    opts.set_class_name("os.activity-monitor");

    if (!g_win.create(opts))
        hsrc::sdk::exit(1);
    (void)refresh_window_options();

    g_win.show(true);
    g_win.focus();
    paint();
    (void)hsrc::sdk::present();
    refresh_monitor(false);
    if (g_dirty) {
        paint();
        (void)hsrc::sdk::present();
    }

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

        (void)refresh_window_options();

        Input in{};
        if (hsrc::sdk::input(in)) {
            const uint8_t pressed = (uint8_t)(in.buttons & ~g_prev_input.buttons);
            if (pressed & UGX_BTN_LEFT) {
                const bool interactive = !g_win_opts.minimized && g_win_opts.visible;
                const int lx = in.mouse_x - g_win_opts.x;
                const int ly = in.mouse_y - g_win_opts.y;
                const bool over = interactive &&
                                  lx >= 0 && ly >= 0 &&
                                  lx < g_win_opts.w && ly < g_win_opts.h;
                if (over || (interactive && in.focus_id == g_win.id()))
                    handle_click(in);
            }
            g_prev_input = in;
        }

        /* Slow on-demand sample — never chase every kernel seq bump. */
        if (!g_win_opts.minimized) {
            g_sample_poll++;
            if (g_sample_poll >= kSampleEvery) {
                g_sample_poll = 0;
                refresh_monitor(true);
            }
        }

        if (g_dirty && !g_win_opts.minimized) {
            paint();
            (void)hsrc::sdk::present();
        }
        hsrc::sdk::yield();
    }
}
