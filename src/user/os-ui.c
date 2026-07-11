#include <user/gx.h>
#include <kernel/syscall.h>
#include <kernel/string.h>

/*
 * os-ui — userspace shell.
 * Draws menubar / dock / settings via ugx; kernel only composites + presents.
 */

#define MENUBAR_H   28
#define DOCK_H      64
#define DOCK_PAD    16
#define DOCK_ICON   44
#define SETTINGS_W  420
#define SETTINGS_H  280

static int screen_w;
static int screen_h;

static int win_menubar = -1;
static int win_dock = -1;
static int win_settings = -1;

static ugx_map map_menubar;
static ugx_map map_dock;
static ugx_map map_settings;

static int settings_open;
static int settings_anim;   /* 0..256 ease */
static int dock_bounce;     /* 0..24 px */
static int dock_bounce_dir;
static int tick;
static uint8_t prev_buttons;

static int clampi(int v, int lo, int hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

static int lerp(int a, int b, int t256)
{
    return a + ((b - a) * t256) / 256;
}

static int ease_out(int t256)
{
    /* quadratic ease-out approx */
    int u = 256 - t256;
    return 256 - (u * u) / 256;
}

static void paint_menubar(void)
{
    ugx_buf_clear(&map_menubar, UGX_RGBA(245, 245, 247, 210));
    ugx_buf_text(&map_menubar, 14, 10, "mykernel", UGX_RGB(30, 30, 32));
    ugx_buf_text(&map_menubar, 100, 10, "File", UGX_RGB(60, 60, 64));
    ugx_buf_text(&map_menubar, 148, 10, "Edit", UGX_RGB(60, 60, 64));
    ugx_buf_text(&map_menubar, 196, 10, "View", UGX_RGB(60, 60, 64));
    ugx_buf_text(&map_menubar, screen_w - 120, 10, "Settings", UGX_RGB(35, 131, 226));
    ugx_damage();
}

static void paint_dock_icon(ugx_map *m, int x, int y, int size, uint32_t fill, const char *label)
{
    ugx_fill_round(win_dock, x, y, size, size, 10, fill);
    if (label)
        ugx_buf_text(m, x + 8, y + size / 2 - 4, label, UGX_RGB(255, 255, 255));
}

static void paint_dock(void)
{
    int w = (int)map_dock.width;
    int h = (int)map_dock.height;
    ugx_buf_clear(&map_dock, UGX_RGBA(255, 255, 255, 40));

    int icons = 3;
    int total = icons * DOCK_ICON + (icons - 1) * 12;
    int x0 = (w - total) / 2;
    int y0 = (h - DOCK_ICON) / 2 - dock_bounce;

    paint_dock_icon(&map_dock, x0, y0, DOCK_ICON, UGX_RGB(90, 160, 255), "App");
    paint_dock_icon(&map_dock, x0 + DOCK_ICON + 12, y0, DOCK_ICON, UGX_RGB(80, 200, 120), "Term");
    paint_dock_icon(&map_dock, x0 + 2 * (DOCK_ICON + 12), y0, DOCK_ICON, UGX_RGB(120, 120, 130), "Set");

    ugx_damage();
}

static void paint_settings(int focused)
{
    ugx_buf_clear(&map_settings, UGX_RGBA(250, 250, 252, 235));

    uint32_t bar = focused ? UGX_RGB(45, 45, 48) : UGX_RGB(70, 70, 74);
    ugx_fill(win_settings, 0, 0, (int)map_settings.width, 28, bar);
    /* traffic lights */
    ugx_fill_round(win_settings, 12, 8, 12, 12, 6, UGX_RGB(255, 95, 87));
    ugx_fill_round(win_settings, 30, 8, 12, 12, 6, UGX_RGB(255, 189, 46));
    ugx_fill_round(win_settings, 48, 8, 12, 12, 6, UGX_RGB(40, 200, 64));
    ugx_buf_text(&map_settings, 72, 10, "Settings", UGX_RGB(240, 240, 242));

    ugx_buf_text(&map_settings, 24, 56, "Appearance", UGX_RGB(40, 40, 44));
    ugx_fill_round(win_settings, 24, 80, 160, 36, 8, UGX_RGB(35, 131, 226));
    ugx_buf_text(&map_settings, 48, 92, "Dark mode", UGX_RGB(255, 255, 255));

    ugx_buf_text(&map_settings, 24, 140, "Desktop", UGX_RGB(40, 40, 44));
    ugx_buf_text(&map_settings, 24, 164, "Managed by os-ui (userspace)", UGX_RGB(110, 110, 115));
    ugx_buf_text(&map_settings, 24, 188, "Kernel: compositor + present", UGX_RGB(110, 110, 115));

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
    a.style = UGX_STYLE_ACRYLIC | UGX_STYLE_NO_DRAG | UGX_STYLE_NO_TITLE | UGX_STYLE_BACKGROUND;
    a.radius = 0;
    strncpy(a.title, "menubar", sizeof(a.title) - 1);
    win_menubar = ugx_wm_create(&a);
    if (win_menubar < 0 || ugx_wm_map(win_menubar, &map_menubar) < 0)
        return -1;

    int dock_w = screen_w - 2 * DOCK_PAD;
    if (dock_w > 420)
        dock_w = 420;
    memset(&a, 0, sizeof(a));
    a.x = (screen_w - dock_w) / 2;
    a.y = screen_h - DOCK_H - 12;
    a.w = dock_w;
    a.h = DOCK_H;
    a.style = UGX_STYLE_ACRYLIC | UGX_STYLE_ROUNDED | UGX_STYLE_NO_DRAG | UGX_STYLE_NO_TITLE | UGX_STYLE_BACKGROUND;
    a.radius = 18;
    strncpy(a.title, "dock", sizeof(a.title) - 1);
    win_dock = ugx_wm_create(&a);
    if (win_dock < 0 || ugx_wm_map(win_dock, &map_dock) < 0)
        return -1;

    memset(&a, 0, sizeof(a));
    a.x = (screen_w - SETTINGS_W) / 2;
    a.y = (screen_h - SETTINGS_H) / 2;
    a.w = SETTINGS_W;
    a.h = SETTINGS_H;
    a.style = UGX_STYLE_ACRYLIC | UGX_STYLE_ROUNDED;
    a.radius = 14;
    strncpy(a.title, "Settings", sizeof(a.title) - 1);
    win_settings = ugx_wm_create(&a);
    if (win_settings < 0 || ugx_wm_map(win_settings, &map_settings) < 0)
        return -1;

    ugx_wm_show(win_settings, 0);
    settings_open = 0;
    settings_anim = 0;
    return 0;
}

static void open_settings(void)
{
    settings_open = 1;
    settings_anim = 0;
    dock_bounce = 0;
    dock_bounce_dir = 1;
    ugx_wm_show(win_settings, 1);
    ugx_wm_focus(win_settings);
    paint_settings(1);
}

static void close_settings(void)
{
    settings_open = 0;
    settings_anim = 0;
    ugx_wm_show(win_settings, 0);
}

static void update_settings_anim(void)
{
    if (!settings_open && settings_anim == 0 && !dock_bounce_dir)
        return;

    if (settings_open && settings_anim < 256) {
        settings_anim = clampi(settings_anim + 18, 0, 256);
        int e = ease_out(settings_anim);
        int target_y = (screen_h - SETTINGS_H) / 2;
        int start_y = target_y + 40;
        int y = lerp(start_y, target_y, e);
        int x = (screen_w - SETTINGS_W) / 2;
        ugx_wm_move(win_settings, x, y);
    }

    if (dock_bounce_dir) {
        dock_bounce += dock_bounce_dir * 2;
        if (dock_bounce >= 10)
            dock_bounce_dir = -1;
        if (dock_bounce <= 0) {
            dock_bounce = 0;
            dock_bounce_dir = 0;
        }
        paint_dock();
    }
}

static void handle_click(int x, int y)
{
    /* menubar "Settings" */
    if (y < MENUBAR_H && x >= screen_w - 120) {
        if (settings_open)
            close_settings();
        else
            open_settings();
        return;
    }

    /* dock icons */
    int dock_w = (int)map_dock.width;
    int dock_x = (screen_w - dock_w) / 2;
    int dock_y = screen_h - DOCK_H - 12;
    if (x >= dock_x && x < dock_x + dock_w && y >= dock_y && y < dock_y + DOCK_H) {
        int lx = x - dock_x;
        int icons = 3;
        int total = icons * DOCK_ICON + (icons - 1) * 12;
        int x0 = (dock_w - total) / 2;
        int idx = -1;
        for (int i = 0; i < icons; i++) {
            int ix = x0 + i * (DOCK_ICON + 12);
            if (lx >= ix && lx < ix + DOCK_ICON)
                idx = i;
        }
        if (idx == 2) {
            open_settings();
        } else {
            dock_bounce = 0;
            dock_bounce_dir = 1;
            paint_dock();
        }
        return;
    }

    if (settings_open) {
        int sx = (screen_w - SETTINGS_W) / 2;
        int sy = (screen_h - SETTINGS_H) / 2;
        if (settings_anim < 256) {
            int e = ease_out(settings_anim);
            sy = lerp(sy + 40, sy, e);
        }
        if (x >= sx + 10 && x < sx + 26 && y >= sy + 6 && y < sy + 22)
            close_settings();
    }
}

void user_os_ui_main(void)
{
    ugx_info info;
    if (ugx_info_get(&info) < 0)
        sys_exit(1);

    screen_w = (int)info.width;
    screen_h = (int)info.height;

    uint32_t bg = UGX_RGB(48, 92, 160);
    ugx_set_wallpaper(&bg, 1, 1, 1);

    if (create_chrome() < 0)
        sys_exit(1);

    paint_menubar();
    paint_dock();
    paint_settings(0);

    prev_buttons = 0;
    tick = 0;
    int last_focus = -2;

    for (;;) {
        ugx_present();
        tick++;

        ugx_input_state in;
        if (ugx_input_get(&in) == 0) {
            uint8_t pressed = (uint8_t)(in.buttons & ~prev_buttons);
            if (pressed & UGX_BTN_LEFT)
                handle_click(in.mouse_x, in.mouse_y);
            prev_buttons = in.buttons;

            if (settings_open && in.focus_id != last_focus) {
                last_focus = in.focus_id;
                paint_settings(in.focus_id == win_settings);
            }
        }

        update_settings_anim();
        sys_yield();
    }
}
