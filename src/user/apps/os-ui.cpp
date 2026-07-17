#include <user/mke.h>
#include <user/sdk/gfx.hpp>
#include <user/sdk/syscall.hpp>

/*
 * HSRC OS — macOS-inspired desktop shell (ring-3 C++ via hsrc::sdk).
 * Single .mke app: menubar + dock + desktop watermark.
 */

namespace {

using hsrc::sdk::Color;
using hsrc::sdk::Input;
using hsrc::sdk::ScreenInfo;
using hsrc::sdk::Surface;
using hsrc::sdk::Window;
using hsrc::sdk::kTransparent;
using hsrc::sdk::rgb;
using hsrc::sdk::rgba;

constexpr int kMenubarH = 28;
constexpr int kDockH    = 72;
constexpr int kDockIcon = 52;
constexpr int kDockGap  = 16;
constexpr int kDockPad  = 20;
constexpr int kDockRad  = 18;
constexpr int kIconRad  = 12;

constexpr Color kDesktop   = rgb(48, 92, 140);
constexpr Color kMenubarBg = rgba(246, 246, 248, 230);
constexpr Color kMenubarFg = rgb(28, 28, 30);
constexpr Color kMenubarDim = rgb(90, 90, 95);
constexpr Color kDockBg    = rgba(40, 40, 45, 220);
constexpr Color kWatermark = rgba(255, 255, 255, 48);

struct DockItem {
    const char *label;
    Color       color;
};

constexpr DockItem kDockItems[] = {
    { "Find", rgb(90, 160, 255) },
    { "Term", rgb(36, 36, 40) },
    { "Files", rgb(255, 190, 60) },
    { "Prefs", rgb(150, 150, 160) },
};

constexpr int kDockCount = (int)(sizeof(kDockItems) / sizeof(kDockItems[0]));

int g_sw = 0;
int g_sh = 0;

Window g_desktop;
Window g_menubar;
Window g_dock;

int g_dock_x = 0;
int g_dock_y = 0;
int g_dock_w = 0;
int g_hover  = -1;
uint8_t g_prev_buttons = 0;

int dock_width()
{
    return kDockCount * kDockIcon + (kDockCount - 1) * kDockGap + kDockPad * 2;
}

void paint_desktop()
{
    Surface &s = g_desktop.surface();
    s.clear(kTransparent);
    /* Soft vignette-ish solid; watermark is the main signal. */
    s.text_centered(g_sw / 2, g_sh / 2 - 10, "HSRC OS", kWatermark, 5);
    s.text_centered(g_sw / 2, g_sh / 2 + 36, "desktop", rgba(255, 255, 255, 36), 2);
    g_desktop.damage();
}

void paint_menubar()
{
    Surface &s = g_menubar.surface();
    s.clear(kMenubarBg);
    s.fill(0, kMenubarH - 1, g_sw, 1, rgb(200, 200, 205));

    /* Fake apple / brand mark */
    s.fill_round(10, 6, 16, 16, 8, rgb(30, 30, 32));
    s.text(32, 10, "HSRC", kMenubarFg, 1);

    const char *menus[] = { "File", "Edit", "View", "Window", "Help" };
    int x = 90;
    for (const char *m : menus) {
        s.text(x, 10, m, kMenubarDim, 1);
        x += 52;
    }

    g_menubar.damage();
}

void paint_dock()
{
    Surface &s = g_dock.surface();
    s.clear(kTransparent);
    s.fill_round(0, 0, g_dock_w, kDockH, kDockRad, kDockBg);
    s.fill(12, 1, g_dock_w - 24, 1, rgba(255, 255, 255, 40));

    const int total = kDockCount * kDockIcon + (kDockCount - 1) * kDockGap;
    const int x0 = (g_dock_w - total) / 2;
    const int y0 = (kDockH - kDockIcon) / 2;

    for (int i = 0; i < kDockCount; i++) {
        const int ix = x0 + i * (kDockIcon + kDockGap);
        const int lift = (i == g_hover) ? 4 : 0;
        const int iy = y0 - lift;
        const int sz = kDockIcon + (i == g_hover ? 4 : 0);

        s.fill_round(ix - (sz - kDockIcon) / 2, iy, sz, sz, kIconRad, kDockItems[i].color);
        s.fill_round(ix - (sz - kDockIcon) / 2 + 6, iy + 4, sz - 12, 12, 6,
                     rgba(255, 255, 255, 55));
        int len = 0;
        while (kDockItems[i].label[len])
            len++;
        s.text(ix + (kDockIcon - len * 8) / 2, iy + sz / 2 - 4,
               kDockItems[i].label, rgb(255, 255, 255), 1);
    }

    g_dock.damage();
}

int dock_hit(int x, int y)
{
    if (x < g_dock_x || y < g_dock_y || x >= g_dock_x + g_dock_w ||
        y >= g_dock_y + kDockH)
        return -1;

    const int lx = x - g_dock_x;
    const int total = kDockCount * kDockIcon + (kDockCount - 1) * kDockGap;
    const int x0 = (g_dock_w - total) / 2;
    for (int i = 0; i < kDockCount; i++) {
        const int ix = x0 + i * (kDockIcon + kDockGap);
        if (lx >= ix && lx < ix + kDockIcon)
            return i;
    }
    return -1;
}

bool build_ui()
{
    if (!hsrc::sdk::set_wallpaper_color(kDesktop))
        return false;

    if (!g_desktop.create(0, 0, g_sw, g_sh,
                          UGX_STYLE_BACKGROUND | UGX_STYLE_NO_DRAG | UGX_STYLE_NO_TITLE |
                              UGX_STYLE_ALPHA,
                          0, "desktop"))
        return false;

    if (!g_menubar.create(0, 0, g_sw, kMenubarH,
                          UGX_STYLE_BACKGROUND | UGX_STYLE_NO_DRAG | UGX_STYLE_NO_TITLE |
                              UGX_STYLE_OPAQUE,
                          0, "menubar"))
        return false;

    g_dock_w = dock_width();
    g_dock_x = (g_sw - g_dock_w) / 2;
    g_dock_y = g_sh - kDockH - 16;

    if (!g_dock.create(g_dock_x, g_dock_y, g_dock_w, kDockH,
                       UGX_STYLE_ROUNDED | UGX_STYLE_NO_DRAG | UGX_STYLE_NO_TITLE |
                           UGX_STYLE_BACKGROUND | UGX_STYLE_ALPHA,
                       kDockRad, "dock"))
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
            hsrc::sdk::yield();
    }

    g_sw = (int)info.width;
    g_sh = (int)info.height;

    if (!build_ui()) {
        (void)hsrc::sdk::set_wallpaper_color(rgb(160, 30, 30));
        (void)hsrc::sdk::present();
        for (;;)
            hsrc::sdk::yield();
    }

    (void)hsrc::sdk::present();

    for (;;) {
        Input in{};
        if (hsrc::sdk::input(in)) {
            const int hover = dock_hit(in.mouse_x, in.mouse_y);
            if (hover != g_hover) {
                g_hover = hover;
                paint_dock();
            }

            const uint8_t pressed = (uint8_t)(in.buttons & ~g_prev_buttons);
            if (pressed & UGX_BTN_LEFT) {
                if (g_hover == 1) {
                    /* Term dock icon → focus existing Terminal window */
                    long tid = hsrc::sdk::syscall1(SYS_WM_FIND, (long)"Terminal");
                    if (tid >= 0) {
                        (void)hsrc::sdk::syscall1(SYS_WM_FOCUS, tid);
                        (void)hsrc::sdk::syscall2(SYS_WM_SHOW, tid, 1);
                    }
                }
                if (g_hover >= 0)
                    paint_dock();
            }
            g_prev_buttons = in.buttons;
        }

        (void)hsrc::sdk::present();
        hsrc::sdk::yield();
    }
}
