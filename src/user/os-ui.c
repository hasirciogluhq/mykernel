#include <user/gx.h>
#include <user/apps.h>
#include <kernel/syscall.h>
#include <kernel/string.h>

/*
 * os-ui — desktop shell (menubar + dock). Apps are separate modules.
 * Kernel only composites; this process draws all chrome via ugx.
 */

#define MENUBAR_H 28
#define DOCK_H    68
#define DOCK_ICON 48
#define DOCK_GAP  14

static int screen_w;
static int screen_h;

static int win_menubar = -1;
static int win_dock = -1;

static ugx_map map_menubar;
static ugx_map map_dock;

static int dock_x, dock_y, dock_w;
static uint8_t prev_buttons;

static void paint_menubar(void)
{
    ugx_fill(win_menubar, 0, 0, screen_w, MENUBAR_H, UGX_RGB(246, 246, 246));
    ugx_buf_fill(&map_menubar, 0, MENUBAR_H - 1, screen_w, 1, UGX_RGB(200, 200, 200));
    ugx_buf_text(&map_menubar, 14, 10, "mykernel", UGX_RGB(20, 20, 20));
    ugx_buf_text(&map_menubar, 100, 10, "File", UGX_RGB(50, 50, 50));
    ugx_buf_text(&map_menubar, 148, 10, "Edit", UGX_RGB(50, 50, 50));
    ugx_buf_text(&map_menubar, 196, 10, "View", UGX_RGB(50, 50, 50));
    ugx_buf_text(&map_menubar, 244, 10, "Window", UGX_RGB(50, 50, 50));
    ugx_buf_text(&map_menubar, 316, 10, "Help", UGX_RGB(50, 50, 50));
    ugx_damage();
}

static void dock_icon(int x, int y, uint32_t color, const char *label)
{
    ugx_fill_round(win_dock, x, y, DOCK_ICON, DOCK_ICON, 12, color);
    /* gloss */
    ugx_fill_round(win_dock, x + 6, y + 4, DOCK_ICON - 12, 14, 6, UGX_RGBA(255, 255, 255, 60));
    if (label)
        ugx_buf_text(&map_dock, x + (DOCK_ICON - (int)strlen(label) * 8) / 2,
                     y + DOCK_ICON / 2 - 4, label, UGX_RGB(255, 255, 255));
}

static void paint_dock(void)
{
    /* solid dock so it stays visible without alpha blending */
    ugx_fill(win_dock, 0, 0, dock_w, DOCK_H, UGX_RGB(55, 55, 60));
    ugx_buf_fill(&map_dock, 8, 1, dock_w - 16, 1, UGX_RGB(120, 120, 130));

    int icons = 4;
    int total = icons * DOCK_ICON + (icons - 1) * DOCK_GAP;
    int x0 = (dock_w - total) / 2;
    int y0 = (DOCK_H - DOCK_ICON) / 2;

    dock_icon(x0 + 0 * (DOCK_ICON + DOCK_GAP), y0, UGX_RGB(90, 160, 255), "Find");
    dock_icon(x0 + 1 * (DOCK_ICON + DOCK_GAP), y0, UGX_RGB(28, 28, 30), "Term");
    dock_icon(x0 + 2 * (DOCK_ICON + DOCK_GAP), y0, UGX_RGB(255, 180, 50), "Note");
    dock_icon(x0 + 3 * (DOCK_ICON + DOCK_GAP), y0, UGX_RGB(140, 140, 150), "Sys");

    ugx_damage();
}

static int create_chrome(void)
{
    ugx_win_create a;

    memset(&a, 0, sizeof(a));
    a.x = 0;
    a.y = 0;
    a.w = screen_w;
    a.h = MENUBAR_H;
    a.style = UGX_STYLE_OPAQUE | UGX_STYLE_NO_DRAG | UGX_STYLE_NO_TITLE | UGX_STYLE_BACKGROUND;
    a.radius = 0;
    strncpy(a.title, "menubar", sizeof(a.title) - 1);
    win_menubar = ugx_wm_create(&a);
    if (win_menubar < 0 || ugx_wm_map(win_menubar, &map_menubar) < 0)
        return -1;

    dock_w = 4 * DOCK_ICON + 3 * DOCK_GAP + 40;
    dock_x = (screen_w - dock_w) / 2;
    dock_y = screen_h - DOCK_H - 14;

    memset(&a, 0, sizeof(a));
    a.x = dock_x;
    a.y = dock_y;
    a.w = dock_w;
    a.h = DOCK_H;
    a.style = UGX_STYLE_ROUNDED | UGX_STYLE_NO_DRAG | UGX_STYLE_NO_TITLE | UGX_STYLE_BACKGROUND;
    a.radius = 16;
    strncpy(a.title, "dock", sizeof(a.title) - 1);
    win_dock = ugx_wm_create(&a);
    if (win_dock < 0 || ugx_wm_map(win_dock, &map_dock) < 0)
        return -1;

    return 0;
}

static int dock_hit(int x, int y)
{
    if (x < dock_x || y < dock_y || x >= dock_x + dock_w || y >= dock_y + DOCK_H)
        return -1;
    int lx = x - dock_x;
    int icons = 4;
    int total = icons * DOCK_ICON + (icons - 1) * DOCK_GAP;
    int x0 = (dock_w - total) / 2;
    for (int i = 0; i < icons; i++) {
        int ix = x0 + i * (DOCK_ICON + DOCK_GAP);
        if (lx >= ix && lx < ix + DOCK_ICON)
            return i;
    }
    return -1;
}

static void handle_click(int x, int y)
{
    int icon = dock_hit(x, y);
    if (icon == 1)
        app_terminal_open(screen_w, screen_h);
    else if (icon == 2)
        app_notepad_open(screen_w, screen_h);
    else if (icon == 3) {
        /* simple system about via notepad-style — open terminal with uname */
        app_terminal_open(screen_w, screen_h);
    }
}

void user_os_ui_main(void)
{
    ugx_info info;
    if (ugx_info_get(&info) < 0) {
        for (;;)
            ;
    }

    screen_w = (int)info.width;
    screen_h = (int)info.height;

    uint32_t bg = UGX_RGB(58, 110, 165);
    ugx_set_wallpaper(&bg, 1, 1, 1);

    if (create_chrome() < 0) {
        /* hard fail color — red means chrome create failed */
        uint32_t err = UGX_RGB(160, 30, 30);
        ugx_set_wallpaper(&err, 1, 1, 1);
        ugx_present();
        for (;;)
            ;
    }

    paint_menubar();
    paint_dock();

    /* Launch example apps on boot so desktop is not empty */
    app_terminal_open(screen_w, screen_h);
    app_notepad_open(screen_w, screen_h);

    ugx_present();
    prev_buttons = 0;

    for (;;) {
        ugx_present();

        ugx_input_state in;
        if (ugx_input_get(&in) == 0) {
            uint8_t pressed = (uint8_t)(in.buttons & ~prev_buttons);
            if (pressed & UGX_BTN_LEFT)
                handle_click(in.mouse_x, in.mouse_y);
            prev_buttons = in.buttons;

            app_terminal_tick(&in, pressed);
            app_notepad_tick(&in, pressed);
        } else {
            ugx_input_state z;
            memset(&z, 0, sizeof(z));
            app_terminal_tick(&z, 0);
            app_notepad_tick(&z, 0);
        }
    }
}
