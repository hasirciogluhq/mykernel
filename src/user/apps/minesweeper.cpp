#include <user/mke.h>
#include <user/sdk/gfx.hpp>
#include <user/sdk/settings.hpp>
#include <user/sdk/thread.hpp>
#include <user/sdk/time.hpp>

/*
 * Minesweeper — classic 9×9 / 10 mines on MKDX/ugx.
 * Left click reveal · right click flag · [new] resets.
 */

namespace {

using hsrc::sdk::Color;
using hsrc::sdk::GxDevice;
using hsrc::sdk::Input;
using hsrc::sdk::ScreenInfo;
using hsrc::sdk::Surface;
using hsrc::sdk::Window;
using hsrc::sdk::WindowOptions;
using hsrc::sdk::kChromeTitleH;
using hsrc::sdk::kUIFontH;
using hsrc::sdk::rgb;
using hsrc::sdk::settings::kThemeWaitTicks;
using hsrc::sdk::settings::refresh_theme;
using hsrc::sdk::settings::theme;

constexpr int kCols = 9;
constexpr int kRows = 9;
constexpr int kMines = 10;
constexpr int kCell = 42;
constexpr int kGap = 3; /* soft gap between tiles — no chunky grid lines */
constexpr int kPad = 16;
constexpr int kHeaderH = 44;
constexpr int kFooterH = 32;
constexpr int kNewBtnW = 64;
constexpr int kNewBtnH = 24;
constexpr int kGridX = kPad;
constexpr int kGridY = kChromeTitleH + kHeaderH;
constexpr int kWinW = kPad * 2 + kCols * kCell + (kCols - 1) * kGap;
constexpr int kWinH = kGridY + kRows * kCell + (kRows - 1) * kGap + kFooterH + kPad;

enum CellVis : uint8_t { Hidden = 0, Revealed = 1, Flagged = 2 };

Window g_win;
GxDevice g_gx;
WindowOptions g_win_opts;
ScreenInfo g_screen{};
Input g_prev{};
bool g_dirty = true;

bool g_mine[kRows][kCols];
uint8_t g_adj[kRows][kCols];
CellVis g_vis[kRows][kCols];
bool g_started = false;
bool g_dead = false;
bool g_won = false;
int g_flags = 0;
uint32_t g_rng = 1;

uint32_t rng_next()
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    if (g_rng == 0)
        g_rng = 0xA5A5u;
    return g_rng;
}

void clear_board()
{
    for (int r = 0; r < kRows; r++) {
        for (int c = 0; c < kCols; c++) {
            g_mine[r][c] = false;
            g_adj[r][c] = 0;
            g_vis[r][c] = Hidden;
        }
    }
    g_started = false;
    g_dead = false;
    g_won = false;
    g_flags = 0;
}

void place_mines(int safe_r, int safe_c)
{
    int placed = 0;
    while (placed < kMines) {
        const int r = (int)(rng_next() % (uint32_t)kRows);
        const int c = (int)(rng_next() % (uint32_t)kCols);
        if (g_mine[r][c])
            continue;
        if (r == safe_r && c == safe_c)
            continue;
        g_mine[r][c] = true;
        placed++;
    }

    for (int r = 0; r < kRows; r++) {
        for (int c = 0; c < kCols; c++) {
            if (g_mine[r][c]) {
                g_adj[r][c] = 0;
                continue;
            }
            int n = 0;
            for (int dr = -1; dr <= 1; dr++) {
                for (int dc = -1; dc <= 1; dc++) {
                    if (dr == 0 && dc == 0)
                        continue;
                    const int rr = r + dr;
                    const int cc = c + dc;
                    if (rr < 0 || cc < 0 || rr >= kRows || cc >= kCols)
                        continue;
                    if (g_mine[rr][cc])
                        n++;
                }
            }
            g_adj[r][c] = (uint8_t)n;
        }
    }
    g_started = true;
}

void check_win()
{
    if (g_dead)
        return;
    int hidden = 0;
    for (int r = 0; r < kRows; r++) {
        for (int c = 0; c < kCols; c++) {
            if (g_vis[r][c] != Revealed)
                hidden++;
        }
    }
    if (hidden == kMines)
        g_won = true;
}

void flood_reveal(int r, int c)
{
    if (r < 0 || c < 0 || r >= kRows || c >= kCols)
        return;
    if (g_vis[r][c] != Hidden)
        return;
    if (g_mine[r][c])
        return;

    g_vis[r][c] = Revealed;
    if (g_adj[r][c] != 0)
        return;

    for (int dr = -1; dr <= 1; dr++) {
        for (int dc = -1; dc <= 1; dc++) {
            if (dr == 0 && dc == 0)
                continue;
            flood_reveal(r + dr, c + dc);
        }
    }
}

void reveal_cell(int r, int c)
{
    if (g_dead || g_won)
        return;
    if (r < 0 || c < 0 || r >= kRows || c >= kCols)
        return;
    if (g_vis[r][c] != Hidden)
        return;

    if (!g_started)
        place_mines(r, c);

    if (g_mine[r][c]) {
        g_vis[r][c] = Revealed;
        g_dead = true;
        for (int rr = 0; rr < kRows; rr++) {
            for (int cc = 0; cc < kCols; cc++) {
                if (g_mine[rr][cc])
                    g_vis[rr][cc] = Revealed;
            }
        }
        return;
    }

    flood_reveal(r, c);
    check_win();
}

void toggle_flag(int r, int c)
{
    if (g_dead || g_won)
        return;
    if (r < 0 || c < 0 || r >= kRows || c >= kCols)
        return;
    if (g_vis[r][c] == Revealed)
        return;

    if (g_vis[r][c] == Flagged) {
        g_vis[r][c] = Hidden;
        g_flags--;
    } else {
        g_vis[r][c] = Flagged;
        g_flags++;
    }
}

bool hit_new(int lx, int ly)
{
    const int bx = kWinW - kPad - kNewBtnW;
    const int by = kChromeTitleH + 10;
    return lx >= bx && lx < bx + kNewBtnW && ly >= by && ly < by + kNewBtnH;
}

bool cell_at(int lx, int ly, int *out_r, int *out_c)
{
    const int gx = lx - kGridX;
    const int gy = ly - kGridY;
    if (gx < 0 || gy < 0)
        return false;
    const int stride = kCell + kGap;
    const int c = gx / stride;
    const int r = gy / stride;
    if (r < 0 || c < 0 || r >= kRows || c >= kCols)
        return false;
    const int ox = gx - c * stride;
    const int oy = gy - r * stride;
    if (ox >= kCell || oy >= kCell)
        return false;
    *out_r = r;
    *out_c = c;
    return true;
}

Color digit_color(int n)
{
    switch (n) {
    case 1: return rgb(70, 130, 220);
    case 2: return rgb(70, 160, 90);
    case 3: return rgb(210, 70, 70);
    case 4: return rgb(100, 70, 180);
    case 5: return rgb(160, 80, 40);
    case 6: return rgb(40, 150, 160);
    case 7: return rgb(50, 50, 55);
    case 8: return rgb(110, 110, 120);
    default: return theme().text;
    }
}

/* Vector digits — scaled 8×8 bitmap looks crunchy at large cells. */
void hbar(Surface &s, int x, int y, int w, int th, Color c)
{
    s.fill_round(x, y, w, th, th / 2, c);
}

void vbar(Surface &s, int x, int y, int h, int th, Color c)
{
    s.fill_round(x, y, th, h, th / 2, c);
}

void draw_digit(Surface &s, int cx, int cy, int digit, Color c)
{
    const int w = 16;
    const int h = 24;
    const int th = 3;
    const int x0 = cx - w / 2;
    const int y0 = cy - h / 2;
    const int mid = y0 + h / 2 - th / 2;

    const bool A = (digit != 1 && digit != 4);
    const bool B = (digit != 5 && digit != 6);
    const bool C = (digit != 2);
    const bool D = (digit != 1 && digit != 4 && digit != 7);
    const bool E = (digit == 2 || digit == 6 || digit == 8);
    const bool F = (digit != 1 && digit != 2 && digit != 3 && digit != 7);
    const bool G = (digit != 1 && digit != 7);

    if (A)
        hbar(s, x0 + th, y0, w - 2 * th, th, c);
    if (G)
        hbar(s, x0 + th, mid, w - 2 * th, th, c);
    if (D)
        hbar(s, x0 + th, y0 + h - th, w - 2 * th, th, c);
    if (F)
        vbar(s, x0, y0 + th, mid - y0 - th / 2, th, c);
    if (B)
        vbar(s, x0 + w - th, y0 + th, mid - y0 - th / 2, th, c);
    if (E)
        vbar(s, x0, mid + th, y0 + h - th - (mid + th), th, c);
    if (C)
        vbar(s, x0 + w - th, mid + th, y0 + h - th - (mid + th), th, c);
}

void draw_flag(Surface &s, int cx, int cy, Color pole, Color cloth)
{
    const int x = cx - 2;
    const int y = cy - 12;
    s.fill(x, y, 3, 24, pole);
    s.fill(x + 3, y + 2, 12, 3, cloth);
    s.fill(x + 3, y + 5, 10, 3, cloth);
    s.fill(x + 3, y + 8, 7, 3, cloth);
    s.fill(x - 3, y + 22, 10, 3, pole);
}

void draw_mine(Surface &s, int cx, int cy, Color body)
{
    s.fill_round(cx - 8, cy - 8, 16, 16, 8, body);
    s.fill(cx - 2, cy - 14, 4, 28, body);
    s.fill(cx - 14, cy - 2, 28, 4, body);
    s.fill(cx - 10, cy - 10, 4, 4, body);
    s.fill(cx + 6, cy - 10, 4, 4, body);
    s.fill(cx - 10, cy + 6, 4, 4, body);
    s.fill(cx + 6, cy + 6, 4, 4, body);
    s.fill_round(cx - 3, cy - 3, 4, 4, 2, rgb(255, 255, 255));
}

void paint()
{
    if (!g_win.ok())
        return;

    Surface &s = g_win.surface();
    const auto &t = theme();

    s.fill(0, kChromeTitleH, kWinW, kWinH - kChromeTitleH, t.bg);

    char line[48];
    {
        int left = kMines - g_flags;
        char *p = line;
        const char *prefix = "mines ";
        while (*prefix)
            *p++ = *prefix++;
        if (left < 0) {
            *p++ = '-';
            left = -left;
        }
        char dig[8];
        int di = 0;
        int v = left;
        if (v == 0)
            dig[di++] = '0';
        while (v > 0 && di < 7) {
            dig[di++] = (char)('0' + (v % 10));
            v /= 10;
        }
        while (di > 0)
            *p++ = dig[--di];
        *p = 0;
    }
    s.text(kPad, kChromeTitleH + 14, line, t.text_dim, 1);

    const char *status = "left: dig  right: flag";
    if (g_dead)
        status = "boom — hit [new]";
    else if (g_won)
        status = "cleared!";
    s.text(kPad + 110, kChromeTitleH + 14, status,
           g_won ? t.accent : (g_dead ? t.danger : t.text_soft), 1);

    const int nbx = kWinW - kPad - kNewBtnW;
    const int nby = kChromeTitleH + 10;
    s.fill_round(nbx, nby, kNewBtnW, kNewBtnH, 6, t.accent);
    s.text(nbx + 16, nby + 6, "new", rgb(255, 255, 255), 1);

    const int stride = kCell + kGap;
    for (int r = 0; r < kRows; r++) {
        for (int c = 0; c < kCols; c++) {
            const int x = kGridX + c * stride;
            const int y = kGridY + r * stride;
            const CellVis vis = g_vis[r][c];
            const int cx = x + kCell / 2;
            const int cy = y + kCell / 2;

            Color fill = t.panel;
            if (vis == Revealed)
                fill = t.inset;
            else if (vis == Flagged)
                fill = t.accent_soft;

            s.fill_round(x, y, kCell, kCell, 8, fill);

            if (vis == Flagged) {
                draw_flag(s, cx, cy, t.text_dim, t.danger);
            } else if (vis == Revealed) {
                if (g_mine[r][c])
                    draw_mine(s, cx, cy, t.danger);
                else if (g_adj[r][c] > 0)
                    draw_digit(s, cx, cy, (int)g_adj[r][c],
                               digit_color((int)g_adj[r][c]));
            }
        }
    }

    s.text(kPad, kWinH - kPad - kUIFontH,
           "minesweeper · hsrc-kernel", t.text_soft, 1);

    g_dirty = false;
}

void handle_input(const Input &in)
{
    const uint8_t pressed = (uint8_t)(in.buttons & ~g_prev.buttons);
    if (!(pressed & (UGX_BTN_LEFT | UGX_BTN_RIGHT)))
        return;
    if (in.hit_id != g_win.id())
        return;
    if (g_win_opts.minimized || !g_win_opts.visible)
        return;

    const int lx = in.mouse_x - g_win_opts.x;
    const int ly = in.mouse_y - g_win_opts.y;
    if (lx < 0 || ly < 0 || lx >= g_win_opts.w || ly >= g_win_opts.h)
        return;

    if ((pressed & UGX_BTN_LEFT) && hit_new(lx, ly)) {
        clear_board();
        g_rng ^= (uint32_t)hsrc::sdk::time::mono_ns();
        g_dirty = true;
        return;
    }

    int r = 0, c = 0;
    if (!cell_at(lx, ly, &r, &c))
        return;

    if (pressed & UGX_BTN_RIGHT) {
        toggle_flag(r, c);
        g_dirty = true;
        return;
    }
    if (pressed & UGX_BTN_LEFT) {
        reveal_cell(r, c);
        g_dirty = true;
    }
}

bool refresh_window_options()
{
    WindowOptions cur;
    if (!g_win.get_options(cur))
        return false;
    const bool changed =
        cur.minimized != g_win_opts.minimized ||
        cur.visible != g_win_opts.visible ||
        cur.x != g_win_opts.x || cur.y != g_win_opts.y ||
        cur.w != g_win_opts.w || cur.h != g_win_opts.h;
    g_win_opts = cur;
    if (changed && !cur.minimized)
        g_dirty = true;
    return changed;
}

} // namespace

extern "C" void mke_main(void)
{
    if (!hsrc::sdk::screen_info(g_screen) || g_screen.width == 0 || g_screen.height == 0) {
        for (;;)
            hsrc::sdk::this_thread::sleep_for(1000u);
    }

    (void)refresh_theme();
    g_rng = (uint32_t)hsrc::sdk::time::mono_ns() ^ 0xC0FFEEu;
    clear_board();

    WindowOptions opts;
    opts.x = g_screen.width > (uint32_t)kWinW ? ((int)g_screen.width - kWinW) / 2 : 40;
    opts.y = g_screen.height > (uint32_t)kWinH ? ((int)g_screen.height - kWinH) / 2 : 40;
    opts.w = kWinW;
    opts.h = kWinH;
    opts.radius = 10;
    opts.rounded = true;
    opts.shadow = true;
    opts.resizable = false;
    opts.framed = true;
    opts.closable = true;
    opts.can_minimize = true;
    opts.can_maximize = false;
    opts.accept_focus = true;
    opts.set_title("Minesweeper");
    opts.set_class_name("os.minesweeper");

    if (!g_win.create(opts))
        hsrc::sdk::exit(1);
    if (!g_gx.create(g_win))
        hsrc::sdk::exit(1);
    (void)refresh_window_options();

    g_win.show(true);
    g_win.focus();
    g_gx.set_chrome_colors(theme().chrome, theme().text, theme().border);

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

        const uint32_t wait_to =
            g_win_opts.minimized ? 200u : kThemeWaitTicks;
        Input in = g_gx.wait(wait_to);

        if (!g_gx.dragging())
            handle_input(in);
        g_prev = in;

        if (!g_win_opts.minimized && g_dirty) {
            (void)g_gx.begin_scene();
            paint();
            (void)g_gx.end_scene();
            (void)g_gx.present();
        }
    }
}
