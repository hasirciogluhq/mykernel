#include <user/mke.h>
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
using hsrc::sdk::process::ProcListEntry;
using hsrc::sdk::process::ProcStat;
using hsrc::sdk::process::SysInfo;
using hsrc::sdk::settings::theme;
using hsrc::sdk::settings::refresh_theme;

constexpr int kWinW = 900;
constexpr int kWinH = 560;
constexpr int kHeaderH = 60;
constexpr int kSummaryY = 84;
constexpr int kSummaryH = 72;
constexpr int kSummaryGap = 12;
constexpr int kSummaryW = 260;
constexpr int kListX = 20;
constexpr int kListY = 172;
constexpr int kListW = 860;
constexpr int kListHeaderH = 28;
constexpr int kRowH = 32;
constexpr int kVisibleRows = 10;
constexpr int kDetailY = 514;
constexpr int kFooterBtnW = 72;
constexpr int kTaskBtnW = 112;
constexpr int kRefreshEvery = 24;
constexpr int kMaxEntries = hsrc::sdk::process::kMaxProcesses;
constexpr int kStatusChars = 128;

constexpr int kThemePollEvery = 96;

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
SysInfo g_sysinfo{};
ProcStat g_selected_stat{};
int g_entry_count = 0;
int g_scroll = 0;
int g_refresh_counter = 0;
bool g_has_selected_stat = false;
bool g_dirty = true;
int g_theme_poll = 0;
bool g_running = true;
bool g_summary_open = true;
pid_t g_selected_pid = -1;
pid_t g_self_pid = -1;
uint64_t g_prev_total_ticks = 0;
uint32_t g_total_cpu_pct = 0;
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
        char ch[2];
        ch[0] = tmp[i];
        ch[1] = 0;
        append_text(dst, dst_size, ch);
    }
}

void set_status(const char *text)
{
    copy_text(g_status, sizeof(g_status), text);
}

bool refresh_window_options()
{
    return g_win.get_options(g_win_opts);
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

void sort_entries()
{
    for (int i = 0; i < g_entry_count; i++) {
        for (int j = i + 1; j < g_entry_count; j++) {
            bool swap = false;
            if (g_entries[j].cpu_pct > g_entries[i].cpu_pct) {
                swap = true;
            } else if (g_entries[j].cpu_pct == g_entries[i].cpu_pct &&
                       g_entries[j].proc.mem_bytes > g_entries[i].proc.mem_bytes) {
                swap = true;
            } else if (g_entries[j].cpu_pct == g_entries[i].cpu_pct &&
                       g_entries[j].proc.mem_bytes == g_entries[i].proc.mem_bytes &&
                       g_entries[j].proc.pid < g_entries[i].proc.pid) {
                swap = true;
            }
            if (swap) {
                MonitorEntry tmp = g_entries[i];
                g_entries[i] = g_entries[j];
                g_entries[j] = tmp;
            }
        }
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

void format_bytes_kb(char *out, size_t out_size, uint32_t bytes)
{
    out[0] = 0;
    append_uint(out, out_size, bytes / 1024u);
    append_text(out, out_size, " KB");
}

void format_ticks(char *out, size_t out_size, uint64_t ticks)
{
    out[0] = 0;
    append_uint(out, out_size, narrow_u64(ticks));
    append_text(out, out_size, " ticks");
}

void format_percent(char *out, size_t out_size, uint32_t pct)
{
    out[0] = 0;
    append_uint(out, out_size, pct);
    append_text(out, out_size, "%");
}

void draw_stat_card(Surface &s, int x, const char *title, const char *value, const char *note)
{
    s.fill_round(x, kSummaryY, kSummaryW, kSummaryH, 10, theme().card);
    s.rect(x, kSummaryY, kSummaryW, kSummaryH, theme().border, 1);
    s.text(x + 16, kSummaryY + 14, title, theme().text_dim, 1);
    s.text(x + 16, kSummaryY + 32, value, theme().text, 1);
    s.text(x + 16, kSummaryY + 52, note, theme().text_soft, 1);
}

void refresh_monitor(bool keep_status)
{
    ProcListEntry raw[kMaxEntries];
    SysInfo info{};
    long count = hsrc::sdk::process::proc_list(raw, kMaxEntries);

    if (hsrc::sdk::process::sysinfo(&info) < 0) {
        set_status("System info unavailable.");
        return;
    }
    if (count < 0) {
        set_status("Process list unavailable.");
        return;
    }
    if (count > kMaxEntries)
        count = kMaxEntries;

    uint64_t total_delta = 0;
    uint32_t total_delta32;
    if (info.total_cpu_ticks >= g_prev_total_ticks)
        total_delta = info.total_cpu_ticks - g_prev_total_ticks;
    total_delta32 = narrow_u64(total_delta);
    g_total_cpu_pct = 0;
    if (total_delta32 > 0) {
        g_total_cpu_pct = (total_delta32 * 100u) / (uint32_t)kRefreshEvery;
        if (g_total_cpu_pct > 100)
            g_total_cpu_pct = 100;
    }

    g_sysinfo = info;
    g_entry_count = (int)count;
    for (int i = 0; i < g_entry_count; i++) {
        g_entries[i].proc = raw[i];
        g_entries[i].cpu_pct = 0;
        if (total_delta32 > 0 && g_total_cpu_pct > 0) {
            uint64_t prev = prev_ticks_for(raw[i].pid);
            uint64_t delta = raw[i].cpu_ticks >= prev ? raw[i].cpu_ticks - prev : 0;
            uint32_t delta32 = narrow_u64(delta);
            g_entries[i].cpu_pct = (delta32 * g_total_cpu_pct) / total_delta32;
        }
    }

    sort_entries();
    remember_samples();
    g_prev_total_ticks = info.total_cpu_ticks;

    if (g_selected_pid > 0) {
        if (hsrc::sdk::process::proc_stat(g_selected_pid, &g_selected_stat) == 0) {
            g_has_selected_stat = true;
        } else {
            g_selected_pid = -1;
            g_has_selected_stat = false;
        }
    } else {
        g_has_selected_stat = false;
    }

    if (g_scroll > 0 && g_scroll >= g_entry_count)
        g_scroll = g_entry_count > 0 ? g_entry_count - 1 : 0;
    if (!keep_status)
        set_status("Bounded scheduler sample; refresh is cooperative.");
    g_dirty = true;
}

void paint()
{
    if (!g_win.ok())
        return;

    Surface &s = g_win.surface();
    s.clear(theme().bg);
    s.draw_window_chrome(kWinW, g_win_opts.title, g_win_opts, theme().chrome, theme().text, theme().border);
    s.fill(0, kChromeTitleH + kHeaderH - 1, kWinW, 1, theme().border);
    s.text(78, kChromeTitleH + 12, "Activity Monitor", theme().text, 1);
    s.text(78, kChromeTitleH + 30, "Bounded process view for Wave N", theme().text_dim, 1);

    const bool can_kill = can_end_task();
    const Color task_bg = can_kill ? theme().danger_soft : theme().card;
    const Color task_fg = can_kill ? theme().danger : theme().text_soft;
    s.fill_round(kWinW - 20 - kTaskBtnW, kChromeTitleH + 14, kTaskBtnW, 28, 8, task_bg);
    s.rect(kWinW - 20 - kTaskBtnW, kChromeTitleH + 14, kTaskBtnW, 28, can_kill ? theme().danger_soft : theme().border, 1);
    s.text(kWinW - 20 - kTaskBtnW + 18, kChromeTitleH + 24, "End Task", task_fg, 1);

    char cpu_value[32];
    char ram_value[32];
    char proc_value[32];
    char uptime_value[32];
    format_percent(cpu_value, sizeof(cpu_value), g_total_cpu_pct);
    ram_value[0] = 0;
    append_uint(ram_value, sizeof(ram_value), g_sysinfo.used_ram_bytes / 1024u);
    append_text(ram_value, sizeof(ram_value), " / ");
    append_uint(ram_value, sizeof(ram_value), g_sysinfo.total_ram_bytes / 1024u);
    append_text(ram_value, sizeof(ram_value), " KB");
    proc_value[0] = 0;
    append_uint(proc_value, sizeof(proc_value), g_sysinfo.process_count);
    uptime_value[0] = 0;
    append_uint(uptime_value, sizeof(uptime_value), narrow_u64(g_sysinfo.uptime_ticks));

    const char *summary_mark = g_summary_open ? "v Summary" : "> Summary";
    s.fill_round(20, kSummaryY - 22, 120, 20, 6, theme().hover);
    s.text(28, kSummaryY - 16, summary_mark, theme().text_dim, 1);

    if (g_summary_open) {
        draw_stat_card(s, 20, "CPU Sample", cpu_value, "sampled from cooperative scheduler ticks");
        draw_stat_card(s, 20 + kSummaryW + kSummaryGap, "RAM", ram_value, "kernel heap used / total");
        draw_stat_card(s, 20 + 2 * (kSummaryW + kSummaryGap), "Processes", proc_value, uptime_value);
    }

    s.text(kListX, kListY - 18, "Name", theme().text_dim, 1);
    s.text(kListX + 288, kListY - 18, "PID", theme().text_dim, 1);
    s.text(kListX + 360, kListY - 18, "State", theme().text_dim, 1);
    s.text(kListX + 520, kListY - 18, "CPU", theme().text_dim, 1);
    s.text(kListX + 640, kListY - 18, "Memory", theme().text_dim, 1);
    s.fill_round(kListX, kListY, kListW, kListHeaderH + kVisibleRows * kRowH, 10, theme().card);
    s.rect(kListX, kListY, kListW, kListHeaderH + kVisibleRows * kRowH, theme().border, 1);

    for (int row = 0; row < kVisibleRows; row++) {
        const int index = g_scroll + row;
        const int y = kListY + kListHeaderH + row * kRowH;
        s.fill(kListX + 1, y, kListW - 2, 1, theme().border);
        if (index >= g_entry_count)
            continue;

        const MonitorEntry &entry = g_entries[index];
        const bool selected = entry.proc.pid == g_selected_pid;
        if (selected)
            s.fill_round(kListX + 4, y + 2, kListW - 8, kRowH - 4, 8, theme().accent_soft);
        else
            s.fill_round(kListX + 4, y + 2, kListW - 8, kRowH - 4, 8, theme().hover);

        char pid_text[16];
        char cpu_text[16];
        char mem_text[24];
        copy_text(pid_text, sizeof(pid_text), "");
        append_uint(pid_text, sizeof(pid_text), (uint32_t)entry.proc.pid);
        format_percent(cpu_text, sizeof(cpu_text), entry.cpu_pct);
        format_bytes_kb(mem_text, sizeof(mem_text), entry.proc.mem_bytes);

        s.text(kListX + 12, y + 10, entry.proc.name, selected ? theme().accent : theme().text, 1);
        s.text(kListX + 288, y + 10, pid_text, selected ? theme().accent : theme().text, 1);
        s.text(kListX + 360, y + 10, hsrc::sdk::process::state_name(entry.proc.state),
               selected ? theme().accent : theme().text_dim, 1);
        s.text(kListX + 520, y + 10, cpu_text, selected ? theme().accent : theme().text, 1);
        s.text(kListX + 640, y + 10, mem_text, selected ? theme().accent : theme().text, 1);
    }

    s.fill_round(20, kDetailY - 34, kWinW - 40, 44, 8, theme().card);
    s.rect(20, kDetailY - 34, kWinW - 40, 44, theme().border, 1);
    if (g_has_selected_stat) {
        char detail[256];
        char ticks[40];
        char mem[24];
        detail[0] = 0;
        append_text(detail, sizeof(detail), g_selected_stat.name);
        append_text(detail, sizeof(detail), "  pid=");
        append_uint(detail, sizeof(detail), (uint32_t)g_selected_stat.pid);
        append_text(detail, sizeof(detail), "  state=");
        append_text(detail, sizeof(detail), hsrc::sdk::process::state_name(g_selected_stat.state));
        append_text(detail, sizeof(detail), "  uptime=");
        append_uint(detail, sizeof(detail), narrow_u64(g_selected_stat.uptime_ticks));
        append_text(detail, sizeof(detail), " ticks");
        format_ticks(ticks, sizeof(ticks), g_selected_stat.cpu_ticks);
        format_bytes_kb(mem, sizeof(mem), g_selected_stat.mem_bytes);
        s.text(32, kDetailY - 20, detail, theme().text, 1);
        s.text(32, kDetailY, ticks, theme().text_dim, 1);
        s.text(220, kDetailY, mem, theme().text_dim, 1);
    } else {
        s.text(32, kDetailY - 12, "Select a process to inspect per-process stats.", theme().text_dim, 1);
    }

    s.text(20, kWinH - 20, g_status, theme().text_dim, 1);

    const bool can_prev = g_scroll > 0;
    const bool can_next = g_scroll + kVisibleRows < g_entry_count;
    const int next_x = kWinW - 20 - kFooterBtnW;
    const int prev_x = next_x - 10 - kFooterBtnW;
    s.fill_round(prev_x, kWinH - 28, kFooterBtnW, 22, 6, can_prev ? theme().card : theme().inset);
    s.fill_round(next_x, kWinH - 28, kFooterBtnW, 22, 6, can_next ? theme().card : theme().inset);
    s.rect(prev_x, kWinH - 28, kFooterBtnW, 22, theme().border, 1);
    s.rect(next_x, kWinH - 28, kFooterBtnW, 22, theme().border, 1);
    s.text(prev_x + 19, kWinH - 20, "Prev", can_prev ? theme().text : theme().text_soft, 1);
    s.text(next_x + 19, kWinH - 20, "Next", can_next ? theme().text : theme().text_soft, 1);

    g_win.damage();
    g_dirty = false;
}

int row_hit(int lx, int ly)
{
    if (lx < kListX || lx >= kListX + kListW)
        return -1;
    if (ly < kListY + kListHeaderH || ly >= kListY + kListHeaderH + kVisibleRows * kRowH)
        return -1;
    int row = (ly - (kListY + kListHeaderH)) / kRowH;
    int index = g_scroll + row;
    if (index < 0 || index >= g_entry_count)
        return -1;
    return index;
}

int footer_hit(int lx, int ly)
{
    const int next_x = kWinW - 20 - kFooterBtnW;
    const int prev_x = next_x - 10 - kFooterBtnW;
    if (ly < kWinH - 28 || ly >= kWinH - 6)
        return -1;
    if (lx >= prev_x && lx < prev_x + kFooterBtnW)
        return 0;
    if (lx >= next_x && lx < next_x + kFooterBtnW)
        return 1;
    return -1;
}

bool task_button_hit(int lx, int ly)
{
    const int x = kWinW - 20 - kTaskBtnW;
    const int y = kChromeTitleH + 14;
    return lx >= x && lx < x + kTaskBtnW && ly >= y && ly < y + 28;
}

bool summary_toggle_hit(int lx, int ly)
{
    return lx >= 20 && lx < 140 && ly >= kSummaryY - 22 && ly < kSummaryY - 2;
}

void perform_end_task()
{
    int idx = selected_index();
    if (idx < 0) {
        set_status("Select a process first.");
        g_dirty = true;
        return;
    }
    if (!can_end_task()) {
        set_status("Selected process cannot be terminated here.");
        g_dirty = true;
        return;
    }

    long rc = hsrc::sdk::process::kill(g_entries[idx].proc.pid);
    if (rc < 0) {
        set_status("End Task failed.");
        g_dirty = true;
        return;
    }

    set_status("Process terminated.");
    refresh_monitor(true);
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

    if (summary_toggle_hit(lx, ly)) {
        g_summary_open = !g_summary_open;
        g_dirty = true;
        return;
    }

    if (task_button_hit(lx, ly)) {
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
        g_has_selected_stat = hsrc::sdk::process::proc_stat(g_selected_pid, &g_selected_stat) == 0;
        g_dirty = true;
    }
}

bool build_ui()
{
    WindowOptions opts;
    opts.x = g_screen.width > (uint32_t)kWinW ? ((int)g_screen.width - kWinW) / 2 : 24;
    opts.y = g_screen.height > (uint32_t)kWinH ? ((int)g_screen.height - kWinH) / 2 : 24;
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
    opts.set_title("Activity Monitor");
    opts.set_class_name("os.activity-monitor");

    if (!g_win.create(opts))
        return false;
    return refresh_window_options();
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

    g_self_pid = (pid_t)hsrc::sdk::process::getpid();
    set_status("Collecting process data...");
    (void)refresh_theme();

    if (!build_ui()) {
        for (;;)
            hsrc::sdk::yield();
    }

    g_win.show(true);
    g_win.focus();
    refresh_monitor(false);
    paint();
    (void)hsrc::sdk::present();

    for (;;) {
        if (!g_running || !g_win.ok()) {
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

        g_refresh_counter++;
        if (g_refresh_counter >= kRefreshEvery) {
            g_refresh_counter = 0;
            refresh_monitor(true);
        }

        if (g_dirty) {
            paint();
            (void)hsrc::sdk::present();
        }
        hsrc::sdk::yield();
    }
}
