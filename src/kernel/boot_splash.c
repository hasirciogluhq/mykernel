#include <kernel/boot_splash.h>
#include <kernel/heap.h>
#include <kernel/types.h>
#include <drivers/display.h>
#include <drivers/serial.h>

/* Calm dark canvas + soft accent — Windows-ish, not neon. */
#define SPLASH_BG   0xFF141518u
#define SPLASH_DOT  0xFF7EB8FFu
#define SPLASH_MID  0xFF4A7FB0u
#define SPLASH_DIM  0xFF2E323Au
#define SPLASH_RING 0xFF22252Bu

#define SPINNER_R      28
#define SPINNER_DOTS   8
#define SPINNER_FRAMES 20
#define SPINNER_DOT_R  3

static void put_px(uint32_t *fb, uint32_t stride, uint32_t w, uint32_t h,
                   int x, int y, uint32_t c)
{
    if (x < 0 || y < 0 || (uint32_t)x >= w || (uint32_t)y >= h)
        return;
    fb[(uint32_t)y * stride + (uint32_t)x] = c;
}

static void fill_disc(uint32_t *fb, uint32_t stride, uint32_t w, uint32_t h,
                      int cx, int cy, int r, uint32_t c)
{
    int rr = r * r;
    int y, x;
    for (y = -r; y <= r; y++) {
        for (x = -r; x <= r; x++) {
            if (x * x + y * y <= rr)
                put_px(fb, stride, w, h, cx + x, cy + y, c);
        }
    }
}

static void stroke_ring(uint32_t *fb, uint32_t stride, uint32_t w, uint32_t h,
                        int cx, int cy, int r, int thickness, uint32_t c)
{
    int r_out = r + thickness;
    int r_in = r - thickness;
    int rr_out = r_out * r_out;
    int rr_in = r_in > 0 ? r_in * r_in : 0;
    int y, x;
    for (y = -r_out; y <= r_out; y++) {
        for (x = -r_out; x <= r_out; x++) {
            int d = x * x + y * y;
            if (d <= rr_out && d >= rr_in)
                put_px(fb, stride, w, h, cx + x, cy + y, c);
        }
    }
}

/* Integer cos/sin for 16 phases (Q8: 256 ≈ 1.0). */
static void unit_xy(int phase, int *ox, int *oy)
{
    static const int16_t cos_q[] = {
        256, 237, 181, 98, 0, -98, -181, -237,
        -256, -237, -181, -98, 0, 98, 181, 237
    };
    static const int16_t sin_q[] = {
        0, 98, 181, 237, 256, 237, 181, 98,
        0, -98, -181, -237, -256, -237, -181, -98
    };
    phase &= 15;
    *ox = (int)cos_q[phase];
    *oy = (int)sin_q[phase];
}

static void draw_spinner(uint32_t *fb, uint32_t stride, uint32_t w, uint32_t h,
                         int cx, int cy, int frame)
{
    int i;
    stroke_ring(fb, stride, w, h, cx, cy, SPINNER_R + 6, 1, SPLASH_RING);
    for (i = 0; i < SPINNER_DOTS; i++) {
        int ox, oy;
        int bright = (i + frame) % SPINNER_DOTS;
        uint32_t c;
        int dr;
        if (bright == 0)
            c = SPLASH_DOT;
        else if (bright == 1)
            c = SPLASH_MID;
        else if (bright <= 3)
            c = SPLASH_DIM;
        else
            c = 0xFF1C1E22u;
        unit_xy((frame + i * (16 / SPINNER_DOTS)) & 15, &ox, &oy);
        dr = SPINNER_DOT_R + (bright == 0 ? 1 : 0);
        fill_disc(fb, stride, w, h,
                  cx + (ox * SPINNER_R) / 256,
                  cy + (oy * SPINNER_R) / 256,
                  dr, c);
    }
}

static void splash_delay(void)
{
    volatile uint32_t n = 350000u;
    while (n--)
        __asm__ volatile("pause");
}

void boot_splash_show(void)
{
    display_ops_t *ops = display_active();
    display_mode_t mode;
    uint32_t *fb;
    uint32_t pixels;
    uint32_t i;
    int cx, cy;
    int frame;
    int clear_r;

    if (!ops || !ops->get_mode || !ops->present)
        return;
    if (ops->get_mode(&mode) < 0 || !mode.width || !mode.height)
        return;
    if (mode.bpp != 32 && mode.bytes_per_pixel != 4)
        return;

    pixels = mode.width * mode.height;
    fb = (uint32_t *)kmalloc((size_t)pixels * sizeof(uint32_t));
    if (!fb) {
        klog("[boot] splash: no mem\n");
        return;
    }

    for (i = 0; i < pixels; i++)
        fb[i] = SPLASH_BG;

    cx = (int)mode.width / 2;
    cy = (int)mode.height / 2;
    clear_r = SPINNER_R + SPINNER_DOT_R + 10;

    for (frame = 0; frame < SPINNER_FRAMES; frame++) {
        int y, x;
        for (y = cy - clear_r; y <= cy + clear_r; y++) {
            for (x = cx - clear_r; x <= cx + clear_r; x++)
                put_px(fb, mode.width, mode.width, mode.height, x, y, SPLASH_BG);
        }
        draw_spinner(fb, mode.width, mode.width, mode.height, cx, cy, frame);
        (void)ops->present(fb, mode.width);
        splash_delay();
    }

    /* Leave final frame — UI compositor covers it when ready. */
    kfree(fb);
    klog("[boot] splash done\n");
}
