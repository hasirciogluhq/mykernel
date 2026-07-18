#include <user/mke.h>
#include <user/sdk/gfx.hpp>
#include <user/sdk/process.hpp>
#include <user/sdk/settings.hpp>
#include <user/sdk/sync.hpp>
#include <user/sdk/thread.hpp>
#include <user/sdk/time.hpp>
#include <user/string.h>

/*
 * Activity Monitor — shared proc_page via seqlock; throttled publish + dirty paint.
 */

namespace {

using hsrc::sdk::ChromeHit;
using hsrc::sdk::Color;
using hsrc::sdk::Input;
using hsrc::sdk::ScreenInfo;
using hsrc::sdk::Surface;
using hsrc::sdk::GxDevice;
using hsrc::sdk::Window;
using hsrc::sdk::WindowOptions;
using hsrc::sdk::kGxWaitForever;
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

constexpr int kWinW = 920;
constexpr int kWinH = 520;
constexpr int kPad = 12;
constexpr int kRowH = 22;
constexpr int kVisibleRows = 12;
constexpr int kHeaderLines = 3;
constexpr int kListY = ui_panel_body_top(kHeaderLines);
/* Column X offsets (name, pid, ppid, kind, state, cpu, mem, ticks). */
constexpr int kColName = 12;
constexpr int kColPid = 168;
constexpr int kColPpid = 214;
constexpr int kColKind = 260;
constexpr int kColState = 296;
constexpr int kColCpu = 388;
constexpr int kColMem = 452;
constexpr int kColTicks = 548;
/* User stacks are 8KiB — keep snapshot buffers in BSS, not on the stack. */
constexpr int kMaxEntries = 96;
constexpr int kStatusChars = 128;
/* ~2.5 Hz sample cadence (mono time, not per-yield). */
constexpr uint64_t kSampleIntervalNs = 400000000ull;

struct MonitorEntry {
    ProcListEntry proc{};
    uint32_t cpu_pct = 0;
};

struct PrevSample {
    pid_t pid = 0;
    uint64_t cpu_ticks = 0;
};

Window g_win;
GxDevice g_gx;
WindowOptions g_win_opts;
ScreenInfo g_screen{};
Input g_prev_input{};
MonitorEntry g_entries[kMaxEntries];
PrevSample g_prev_samples[kMaxEntries];
uint32_t g_delta_scratch[kMaxEntries];
ProcListEntry g_snapshot_scratch[kMaxEntries];
SysInfo g_sysinfo{};
ProcStat g_selected_stat{};
int g_entry_count = 0;
int g_scroll = 0;
bool g_has_selected_stat = false;
bool g_dirty = true;
bool g_was_minimized = false;
bool g_need_sample = true;
uint32_t g_last_applied_seq = 0;
uint32_t g_last_applied_gen = 0;
uint64_t g_last_sample_ns = 0;
pid_t g_selected_pid = -1;
pid_t g_self_pid = -1;
uint64_t g_prev_total_ticks = 0;
uint64_t g_prev_idle_ticks = 0;
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

bool prev_sample_tracked(pid_t pid)
{
    for (int i = 0; i < kMaxEntries; i++) {
        if (g_prev_samples[i].pid == pid)
            return true;
    }
    return false;
}

void format_bytes(char *dst, size_t dst_size, uint32_t bytes)
{
    if (!dst || dst_size == 0)
        return;
    dst[0] = 0;
    if (bytes >= 1024u * 1024u) {
        uint32_t whole = bytes / (1024u * 1024u);
        uint32_t frac = ((bytes % (1024u * 1024u)) * 10u) / (1024u * 1024u);
        append_uint(dst, dst_size, whole);
        append_text(dst, dst_size, ".");
        append_uint(dst, dst_size, frac);
        append_text(dst, dst_size, " MiB");
    } else if (bytes >= 1024u) {
        uint32_t kib = (bytes + 512u) / 1024u;
        append_uint(dst, dst_size, kib);
        append_text(dst, dst_size, " KiB");
    } else {
        append_uint(dst, dst_size, bytes);
        append_text(dst, dst_size, " B");
    }
}

void format_u64(char *dst, size_t dst_size, uint64_t value)
{
    char tmp[24];
    int i = 0;
    if (!dst || dst_size == 0)
        return;
    dst[0] = 0;
    if (value == 0) {
        append_text(dst, dst_size, "0");
        return;
    }
    while (value > 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (value % 10ull));
        value /= 10ull;
    }
    while (i-- > 0) {
        char ch[2] = { tmp[i], 0 };
        append_text(dst, dst_size, ch);
    }
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

bool refresh_monitor(bool keep_status, bool force = false)
{
    SysInfo info{};
    int count = 0;
    int live = 0;

    if (!hsrc::sdk::process::map_proc_page()) {
        set_status("proc snapshot unavailable");
        return true;
    }

    const uint32_t gen_before = hsrc::sdk::process::snapshot_generation();
    const uint32_t seq_before = hsrc::sdk::process::snapshot_seq();
    (void)hsrc::sdk::process::poll_snapshot_publish();
    const uint32_t seq_after = hsrc::sdk::process::snapshot_seq();

    if (!force && seq_after == g_last_applied_seq && gen_before == g_last_applied_gen &&
        seq_after == seq_before) {
        return false;
    }

    if (!hsrc::sdk::process::snapshot(g_snapshot_scratch, kMaxEntries, &count, &info)) {
        set_status("proc snapshot unavailable");
        return true;
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

    uint64_t sample_dt = 0;
    if (g_last_sample_ticks > 0 && info.uptime_ticks >= g_last_sample_ticks)
        sample_dt = info.uptime_ticks - g_last_sample_ticks;

    uint32_t sum_delta32 = 0;
    const bool have_baseline = g_last_sample_ticks > 0;
    for (int i = 0; i < count; i++) {
        uint32_t delta32 = 0;
        if (have_baseline && prev_sample_tracked(g_entries[i].proc.pid)) {
            uint64_t prev = prev_ticks_for(g_entries[i].proc.pid);
            if (g_entries[i].proc.cpu_ticks >= prev)
                delta32 = narrow_u64(g_entries[i].proc.cpu_ticks - prev);
        }
        g_delta_scratch[i] = delta32;
        sum_delta32 += delta32;
    }

    uint64_t idle_delta = 0;
    if (have_baseline && info.idle_ticks >= g_prev_idle_ticks)
        idle_delta = info.idle_ticks - g_prev_idle_ticks;

    uint64_t busy_delta = sum_delta32;
    uint64_t denom = busy_delta + idle_delta;
    if (denom == 0 && sample_dt > 0)
        denom = sample_dt;

    uint32_t next_cpu = 0;
    if (have_baseline && denom > 0 && busy_delta > 0) {
        next_cpu = (uint32_t)((busy_delta * 100ull) / denom);
        if (next_cpu > 100)
            next_cpu = 100;
    }

    for (int i = 0; i < count; i++) {
        if (busy_delta > 0 && have_baseline)
            g_entries[i].cpu_pct = (uint32_t)(((uint64_t)g_delta_scratch[i] * 100ull) / busy_delta);
        else
            g_entries[i].cpu_pct = 0;
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
    g_prev_idle_ticks = info.idle_ticks;
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
            g_selected_stat.stack_bytes = e.stack_bytes;
            g_selected_stat.image_bytes = e.image_bytes;
            g_selected_stat.vma_bytes = e.vma_bytes;
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

    g_last_applied_seq = hsrc::sdk::process::snapshot_seq();
    g_last_applied_gen = hsrc::sdk::process::snapshot_generation();
    return true;
}

void paint()
{
    if (!g_win.ok())
        return;

    const auto &t = theme();
    Surface &s = g_win.surface();
    s.fill(0, kChromeTitleH, kWinW, kWinH - kChromeTitleH, t.bg);

    s.text(kPad, ui_panel_text_y(0), "Activity Monitor", t.text, 1);

    char line[192];
    line[0] = 0;
    append_text(line, sizeof(line), "cpu ");
    append_uint(line, sizeof(line), g_total_cpu_pct);
    append_text(line, sizeof(line), "%  ram ");
    format_bytes(line + strlen(line), sizeof(line) - strlen(line), g_sysinfo.used_ram_bytes);
    append_text(line, sizeof(line), "/");
    format_bytes(line + strlen(line), sizeof(line) - strlen(line), g_sysinfo.total_ram_bytes);
    append_text(line, sizeof(line), "  procs ");
    append_uint(line, sizeof(line), g_sysinfo.process_count);
    s.text(kPad, ui_panel_text_y(1), line, t.text_dim, 1);

    const bool can_kill = can_end_task();
    s.text(kWinW - kPad - 72, ui_panel_text_y(0), "[end]",
           can_kill ? t.danger : t.text_soft, 1);

    s.text(kColName, ui_panel_text_y(2), "name", t.text_dim, 1);
    s.text(kColPid, ui_panel_text_y(2), "pid", t.text_dim, 1);
    s.text(kColPpid, ui_panel_text_y(2), "ppid", t.text_dim, 1);
    s.text(kColKind, ui_panel_text_y(2), "kind", t.text_dim, 1);
    s.text(kColState, ui_panel_text_y(2), "state", t.text_dim, 1);
    s.text(kColCpu, ui_panel_text_y(2), "cpu%", t.text_dim, 1);
    s.text(kColMem, ui_panel_text_y(2), "memory", t.text_dim, 1);
    s.text(kColTicks, ui_panel_text_y(2), "ticks", t.text_dim, 1);
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
        char ppid_text[16];
        char cpu_text[16];
        char mem_text[32];
        char tick_text[24];
        pid_text[0] = 0;
        append_uint(pid_text, sizeof(pid_text), (uint32_t)entry.proc.pid);
        ppid_text[0] = 0;
        append_uint(ppid_text, sizeof(ppid_text), (uint32_t)entry.proc.ppid);
        cpu_text[0] = 0;
        append_uint(cpu_text, sizeof(cpu_text), entry.cpu_pct);
        append_text(cpu_text, sizeof(cpu_text), "%");
        mem_text[0] = 0;
        format_bytes(mem_text, sizeof(mem_text), entry.proc.mem_bytes);
        tick_text[0] = 0;
        format_u64(tick_text, sizeof(tick_text), entry.proc.cpu_ticks);

        Color fg = selected ? t.accent : t.text;
        Color dim = selected ? t.accent : t.text_dim;
        s.text(kColName + 4, y + row_text_dy, entry.proc.name, fg, 1);
        s.text(kColPid, y + row_text_dy, pid_text, fg, 1);
        s.text(kColPpid, y + row_text_dy, ppid_text, dim, 1);
        s.text(kColKind, y + row_text_dy, entry.proc.is_user ? "user" : "kern", dim, 1);
        s.text(kColState, y + row_text_dy, hsrc::sdk::process::state_name(entry.proc.state), dim, 1);
        s.text(kColCpu, y + row_text_dy, cpu_text, fg, 1);
        s.text(kColMem, y + row_text_dy, mem_text, fg, 1);
        s.text(kColTicks, y + row_text_dy, tick_text, dim, 1);
    }

    s.fill(kPad, kWinH - 64, kWinW - kPad * 2, 1, t.border);
    if (g_has_selected_stat) {
        char detail[256];
        char stack_txt[24];
        char image_txt[24];
        char vma_txt[24];
        char total_txt[24];
        detail[0] = 0;
        append_text(detail, sizeof(detail), g_selected_stat.name);
        append_text(detail, sizeof(detail), "  pid=");
        append_uint(detail, sizeof(detail), (uint32_t)g_selected_stat.pid);
        append_text(detail, sizeof(detail), "  ppid=");
        append_uint(detail, sizeof(detail), (uint32_t)g_selected_stat.ppid);
        append_text(detail, sizeof(detail), "  ");
        append_text(detail, sizeof(detail), g_selected_stat.is_user ? "user" : "kernel");
        append_text(detail, sizeof(detail), "  ");
        append_text(detail, sizeof(detail), hsrc::sdk::process::state_name(g_selected_stat.state));
        s.text(kPad, kWinH - 56, detail, t.text, 1);

        stack_txt[0] = 0;
        image_txt[0] = 0;
        vma_txt[0] = 0;
        total_txt[0] = 0;
        format_bytes(stack_txt, sizeof(stack_txt), g_selected_stat.stack_bytes);
        format_bytes(image_txt, sizeof(image_txt), g_selected_stat.image_bytes);
        format_bytes(vma_txt, sizeof(vma_txt), g_selected_stat.vma_bytes);
        format_bytes(total_txt, sizeof(total_txt), g_selected_stat.mem_bytes);

        char mem_line[256];
        mem_line[0] = 0;
        append_text(mem_line, sizeof(mem_line), "cpu ticks=");
        format_u64(mem_line + strlen(mem_line), sizeof(mem_line) - strlen(mem_line),
                   g_selected_stat.cpu_ticks);
        append_text(mem_line, sizeof(mem_line), "  stack=");
        append_text(mem_line, sizeof(mem_line), stack_txt);
        append_text(mem_line, sizeof(mem_line), "  image=");
        append_text(mem_line, sizeof(mem_line), image_txt);
        append_text(mem_line, sizeof(mem_line), "  vma=");
        append_text(mem_line, sizeof(mem_line), vma_txt);
        append_text(mem_line, sizeof(mem_line), "  total=");
        append_text(mem_line, sizeof(mem_line), total_txt);
        s.text(kPad, kWinH - 36, mem_line, t.text_dim, 1);
    } else {
        s.text(kPad, kWinH - 48, "select a process", t.text_dim, 1);
    }
    s.text(kPad, kWinH - 16, g_status, t.text_dim, 1);

    const bool can_prev = g_scroll > 0;
    const bool can_next = g_scroll + kVisibleRows < g_entry_count;
    s.text(kWinW - 120, kWinH - 16, can_prev ? "prev" : "    ", t.text_dim, 1);
    s.text(kWinW - 60, kWinH - 16, can_next ? "next" : "    ", t.text_dim, 1);
    /* Present publishes once — no Window::damage here. */
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
    if (ly < kWinH - 20 || ly >= kWinH - 4)
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
    g_need_sample = true;
    if (refresh_monitor(true, true))
        g_dirty = true;
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
        g_selected_stat.stack_bytes = e.stack_bytes;
        g_selected_stat.image_bytes = e.image_bytes;
        g_selected_stat.vma_bytes = e.vma_bytes;
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
            hsrc::sdk::this_thread::sleep_for(1000u);
    }

    g_self_pid = (pid_t)hsrc::sdk::process::getpid();
    set_status("collecting...");
    (void)refresh_theme();
    (void)hsrc::sdk::process::map_proc_page();
    (void)hsrc::sdk::time::init();

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
    if (!g_gx.create(g_win))
        hsrc::sdk::exit(1);
    (void)refresh_window_options();

    g_win.show(true);
    g_win.focus();
    g_gx.set_chrome_colors(theme().chrome, theme().text, theme().border);
    (void)refresh_monitor(false, true);

    for (;;) {
        if (!g_win.ok()) {
            (void)g_win.close();
            hsrc::sdk::exit(0);
        }

        if (refresh_theme()) {
            g_gx.set_chrome_colors(theme().chrome, theme().text, theme().border);
            g_dirty = true;
        }

        (void)refresh_window_options();

        uint32_t wait_to = kThemeWaitTicks;
        if (g_win_opts.minimized)
            wait_to = 200u;

        Input in = g_gx.wait(wait_to);
        const bool dragging = g_gx.dragging();

        if (!dragging) {
            const uint8_t pressed = (uint8_t)(in.buttons & ~g_prev_input.buttons);
            if (pressed & UGX_BTN_LEFT) {
                const bool interactive = !g_win_opts.minimized && g_win_opts.visible;
                if (interactive && in.hit_id == g_win.id())
                    handle_click(in);
            }
            g_prev_input = in;
        } else {
            g_prev_input = in;
        }

        if (!g_win_opts.minimized) {
            if (hsrc::sdk::process::map_proc_page()) {
                const uint32_t gen = hsrc::sdk::process::snapshot_generation();
                if (gen != g_last_applied_gen)
                    g_need_sample = true;
            }

            const uint64_t now_ns = hsrc::sdk::time::mono_ns();
            if (g_need_sample || now_ns - g_last_sample_ns >= kSampleIntervalNs) {
                g_need_sample = false;
                g_last_sample_ns = now_ns;
                if (refresh_monitor(true))
                    g_dirty = true;
            }

            if (g_dirty) {
                (void)g_gx.begin_scene();
                paint();
                (void)g_gx.end_scene();
                (void)g_gx.present();
                g_dirty = false;
            }
        } else {
            g_need_sample = true;
        }
    }
}
