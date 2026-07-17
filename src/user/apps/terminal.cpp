#include <user/mke.h>
#include <user/sdk/gfx.hpp>
#include <user/sdk/fs.hpp>
#include <user/sdk/syscall.hpp>
#include <user/string.h>
#include <kernel/vfs.h>

/*
 * HSRC Terminal — ring-3 shell window (root by default).
 * Builtins: help, clear, pwd, cd, ls, mkdir, cat, touch, echo, whoami, id, uname
 */

namespace {

using hsrc::sdk::Color;
using hsrc::sdk::Input;
using hsrc::sdk::ScreenInfo;
using hsrc::sdk::Surface;
using hsrc::sdk::Window;
using hsrc::sdk::rgb;
using hsrc::sdk::rgba;

constexpr int kWinW = 720;
constexpr int kWinH = 440;
constexpr int kPad = 10;
constexpr int kLineH = 12;
constexpr int kCols = 86;
constexpr int kRows = 32;
constexpr int kHist = 120;

constexpr Color kBg     = rgb(18, 18, 22);
constexpr Color kFg     = rgb(220, 220, 225);
constexpr Color kDim    = rgb(120, 120, 130);
constexpr Color kAccent = rgb(90, 200, 120);
constexpr Color kErr    = rgb(240, 110, 110);
constexpr Color kTitle  = rgb(40, 40, 48);

Window g_win;
char g_lines[kHist][kCols + 1];
Color g_line_color[kHist];
int g_nlines = 0;
char g_cwd[VFS_PATH_MAX];
char g_input[kCols];
int g_inlen = 0;
bool g_dirty = true;

void line_push(const char *text, Color c)
{
    if (g_nlines >= kHist) {
        for (int i = 1; i < kHist; i++) {
            strcpy(g_lines[i - 1], g_lines[i]);
            g_line_color[i - 1] = g_line_color[i];
        }
        g_nlines = kHist - 1;
    }
    strncpy(g_lines[g_nlines], text ? text : "", kCols);
    g_lines[g_nlines][kCols] = 0;
    g_line_color[g_nlines] = c;
    g_nlines++;
    g_dirty = true;
}

void refresh_cwd()
{
    if (hsrc::sdk::getcwd(g_cwd, sizeof(g_cwd)) < 0)
        strcpy(g_cwd, "/");
}

void make_prompt(char *out, size_t n)
{
    /* root@hsrc:/path# */
    const char *user = (hsrc::sdk::geteuid() == 0) ? "root" : "user";
    char tmp[VFS_PATH_MAX + 32];
    int i = 0;
    const char *p;

    p = user;
    while (*p && i < (int)sizeof(tmp) - 1)
        tmp[i++] = *p++;
    const char *mid = "@hsrc:";
    p = mid;
    while (*p && i < (int)sizeof(tmp) - 1)
        tmp[i++] = *p++;
    p = g_cwd;
    while (*p && i < (int)sizeof(tmp) - 1)
        tmp[i++] = *p++;
    tmp[i++] = (hsrc::sdk::geteuid() == 0) ? '#' : '$';
    tmp[i++] = ' ';
    tmp[i] = 0;
    strncpy(out, tmp, n - 1);
    out[n - 1] = 0;
    (void)n;
}

void paint()
{
    Surface &s = g_win.surface();
    s.clear(kBg);
    s.fill(0, 0, kWinW, 22, kTitle);
    s.text(kPad, 7, "Terminal — root shell", kFg, 1);

    const int top = 28;
    const int visible = (kWinH - top - kPad - kLineH) / kLineH;
    int start = g_nlines > visible ? g_nlines - visible : 0;
    int y = top;
    for (int i = start; i < g_nlines; i++) {
        s.text(kPad, y, g_lines[i], g_line_color[i], 1);
        y += kLineH;
    }

    char prompt[160];
    char row[kCols + 1];
    make_prompt(prompt, sizeof(prompt));
    int pi = 0;
    while (prompt[pi] && pi < kCols)
        row[pi] = prompt[pi], pi++;
    for (int i = 0; i < g_inlen && pi < kCols; i++)
        row[pi++] = g_input[i];
    if (pi < kCols)
        row[pi++] = '_';
    row[pi] = 0;
    s.text(kPad, y, row, kAccent, 1);

    g_win.damage();
    g_dirty = false;
}

const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

bool cmd_is(const char *s, const char *name)
{
    size_t n = 0;
    while (name[n])
        n++;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != name[i])
            return false;
    }
    return s[n] == 0 || s[n] == ' ' || s[n] == '\t';
}

const char *cmd_args(const char *s, const char *name)
{
    size_t n = 0;
    while (name[n])
        n++;
    return skip_ws(s + n);
}

void cmd_help()
{
    line_push("builtins: help clear pwd cd ls mkdir cat touch echo whoami id uname", kDim);
}

void cmd_ls(const char *arg)
{
    const char *path = (*arg) ? arg : ".";
    int fd = (int)hsrc::sdk::open(path, O_RDONLY | O_DIRECTORY);
    vfs_dirent_t ents[32];
    long n;
    char row[kCols + 1];

    if (fd < 0) {
        line_push("ls: cannot open directory", kErr);
        return;
    }
    n = hsrc::sdk::getdents(fd, ents, 32);
    hsrc::sdk::close(fd);
    if (n < 0) {
        line_push("ls: readdir failed", kErr);
        return;
    }
    if (n == 0) {
        line_push("(empty)", kDim);
        return;
    }
    for (long i = 0; i < n; i++) {
        int j = 0;
        row[j++] = S_ISDIR(ents[i].type) ? 'd' : '-';
        row[j++] = ' ';
        const char *nm = ents[i].name;
        while (*nm && j < kCols)
            row[j++] = *nm++;
        row[j] = 0;
        line_push(row, S_ISDIR(ents[i].type) ? kAccent : kFg);
    }
}

void cmd_cat(const char *arg)
{
    char buf[128];
    char line[kCols + 1];
    int fd, li = 0;
    long n;

    if (!*arg) {
        line_push("cat: missing path", kErr);
        return;
    }
    fd = (int)hsrc::sdk::open(arg, O_RDONLY);
    if (fd < 0) {
        line_push("cat: open failed", kErr);
        return;
    }
    while ((n = hsrc::sdk::read(fd, buf, sizeof(buf))) > 0) {
        for (long i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n' || li >= kCols) {
                line[li] = 0;
                line_push(line, kFg);
                li = 0;
                if (c == '\n')
                    continue;
            }
            if (c == '\r')
                continue;
            line[li++] = c;
        }
    }
    if (li > 0) {
        line[li] = 0;
        line_push(line, kFg);
    }
    hsrc::sdk::close(fd);
    if (n < 0)
        line_push("cat: read error", kErr);
}

void cmd_touch(const char *arg)
{
    int fd;
    if (!*arg) {
        line_push("touch: missing path", kErr);
        return;
    }
    fd = (int)hsrc::sdk::open(arg, O_WRONLY | O_CREAT);
    if (fd < 0) {
        line_push("touch: failed", kErr);
        return;
    }
    hsrc::sdk::close(fd);
}

void run_line(const char *line)
{
    const char *s = skip_ws(line);
    char arg[VFS_PATH_MAX];
    int ai = 0;

    if (!*s)
        return;

    if (strcmp(s, "help") == 0 || strcmp(s, "?") == 0) {
        cmd_help();
        return;
    }
    if (strcmp(s, "clear") == 0) {
        g_nlines = 0;
        g_dirty = true;
        return;
    }
    if (strcmp(s, "pwd") == 0) {
        refresh_cwd();
        line_push(g_cwd, kFg);
        return;
    }
    if (strcmp(s, "whoami") == 0) {
        line_push(hsrc::sdk::geteuid() == 0 ? "root" : "user", kFg);
        return;
    }
    if (strcmp(s, "id") == 0) {
        if (hsrc::sdk::getuid() == 0 && hsrc::sdk::geteuid() == 0)
            line_push("uid=0(root) euid=0(root)", kFg);
        else
            line_push("uid!=0", kFg);
        return;
    }
    if (strcmp(s, "uname") == 0) {
        line_push("HSRC OS mykernel i386", kFg);
        return;
    }
    if (cmd_is(s, "cd")) {
        const char *rest = cmd_args(s, "cd");
        if (!*rest)
            rest = "/root";
        if (hsrc::sdk::chdir(rest) < 0)
            line_push("cd: no such directory", kErr);
        else
            refresh_cwd();
        return;
    }
    if (cmd_is(s, "ls")) {
        cmd_ls(cmd_args(s, "ls"));
        return;
    }
    if (cmd_is(s, "mkdir")) {
        const char *rest = cmd_args(s, "mkdir");
        if (!*rest) {
            line_push("mkdir: missing operand", kErr);
            return;
        }
        if (hsrc::sdk::mkdir(rest, 0755) < 0)
            line_push("mkdir: failed", kErr);
        return;
    }
    if (cmd_is(s, "cat")) {
        cmd_cat(cmd_args(s, "cat"));
        return;
    }
    if (cmd_is(s, "touch")) {
        cmd_touch(cmd_args(s, "touch"));
        return;
    }
    if (cmd_is(s, "echo")) {
        line_push(cmd_args(s, "echo"), kFg);
        return;
    }

    /* unknown */
    ai = 0;
    while (s[ai] && s[ai] != ' ' && ai < (int)sizeof(arg) - 1) {
        arg[ai] = s[ai];
        ai++;
    }
    arg[ai] = 0;
    {
        char msg[kCols + 1];
        int j = 0;
        const char *p = arg;
        const char *pre = "command not found: ";
        while (*pre && j < kCols)
            msg[j++] = *pre++;
        while (*p && j < kCols)
            msg[j++] = *p++;
        msg[j] = 0;
        line_push(msg, kErr);
    }
}

void on_enter()
{
    char shown[kCols + 1];
    char prompt[160];
    int i = 0;

    make_prompt(prompt, sizeof(prompt));
    while (prompt[i] && i < kCols) {
        shown[i] = prompt[i];
        i++;
    }
    for (int j = 0; j < g_inlen && i < kCols; j++)
        shown[i++] = g_input[j];
    shown[i] = 0;
    line_push(shown, kDim);

    g_input[g_inlen] = 0;
    run_line(g_input);
    g_inlen = 0;
    g_dirty = true;
}

void handle_key(int ch)
{
    if (ch < 0)
        return;
    if (ch == '\n' || ch == '\r') {
        on_enter();
        return;
    }
    if (ch == 8 || ch == 127) { /* backspace */
        if (g_inlen > 0) {
            g_inlen--;
            g_dirty = true;
        }
        return;
    }
    if (ch == 3) { /* ctrl-c */
        line_push("^C", kDim);
        g_inlen = 0;
        g_dirty = true;
        return;
    }
    if (ch >= 32 && ch < 127 && g_inlen < kCols - 1) {
        g_input[g_inlen++] = (char)ch;
        g_dirty = true;
    }
}

} // namespace

extern "C" void mke_main(void)
{
    ScreenInfo info{};
    if (!hsrc::sdk::screen_info(info) || info.width == 0) {
        for (;;)
            hsrc::sdk::yield();
    }

    const int x = ((int)info.width - kWinW) / 2;
    const int y = ((int)info.height - kWinH) / 2;

    if (!g_win.create(x, y, kWinW, kWinH,
                      UGX_STYLE_ROUNDED | UGX_STYLE_OPAQUE, 10, "Terminal")) {
        for (;;)
            hsrc::sdk::yield();
    }

    g_win.show(true);
    g_win.focus();

    /* Start in /root as root */
    (void)hsrc::sdk::chdir("/root");
    refresh_cwd();

    line_push("HSRC Terminal — running as root (uid 0)", kAccent);
    line_push("Type 'help' for builtins.", kDim);

    paint();
    (void)hsrc::sdk::present();

    for (;;) {
        Input in{};
        (void)hsrc::sdk::input(in);

        if (in.focus_id == g_win.id()) {
            for (;;) {
                long k = hsrc::sdk::syscall1(SYS_WM_POP_KEY, g_win.id());
                if (k < 0)
                    break;
                handle_key((int)k);
            }
        }

        if (g_dirty)
            paint();

        (void)hsrc::sdk::present();
        hsrc::sdk::yield();
    }
}
