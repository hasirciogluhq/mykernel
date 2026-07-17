#include <kernel/boot_splash.h>
#include <kernel/heap.h>
#include <kernel/types.h>
#include <drivers/display.h>
#include <drivers/serial.h>

/*
 * Dashboard gauge loader: dark track + bright fill arc with rounded caps.
 * Rotation and fill are independent — the ring spins continuously while fill
 * ping-pongs 0% ↔ 100% with ease-in-out.
 *
 * Ring geometry is rasterized per-pixel with distance-field coverage and
 * angular sector tests (no stamped disc LUT).
 */
#define SPLASH_BG     0xFF000000u
#define SPLASH_TRACK  0xFF2C2C2Eu
#define SPLASH_FILL   0xFFF5F5F7u
#define SPLASH_GLOW   0xFFD1D1D6u

#define RING_R        34
#define RING_THICK    10
#define TIP_R         (RING_THICK / 2)
#define INNER_R       (RING_R - TIP_R)
#define OUTER_R       (RING_R + TIP_R)

#define ANGLE_FULL    65536
#define ANGLE_QTR     (ANGLE_FULL / 4)
#define AA_Q8         256
#define FEATHER_ANG   384          /* ~2.1 deg arc-end AA */
#define MIN_ARC_ANG   (ANGLE_FULL / 200)

#define SPLASH_FRAMES 90
#define FILL_CYCLE    62          /* frames for one 0→100→0 leg */
#define ROT_PER_FRAME 2           /* legacy step count per frame */
#define ROT_SPEED     ((ROT_PER_FRAME * ANGLE_FULL) / 64)

static int clampi(int v, int lo, int hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

/* t_q8: 0..256 */
static int lerpi(int a, int b, int t_q8)
{
    return a + ((b - a) * clampi(t_q8, 0, 256)) / 256;
}

/* smoothstep ease-in-out, input/output 0..256 (Q8) */
static int ease_in_out_q8(int t_q8)
{
    int t = clampi(t_q8, 0, 256);
    long long num = (long long)t * t * (3 * 256 - 2 * t);

    return (int)(num / (256LL * 256LL));
}

/* 0→256→0 over cycle_len frames */
static int pingpong_q8(int frame, int cycle_len)
{
    int half;
    int pos;
    int t;

    if (cycle_len < 2)
        return 0;

    half = cycle_len / 2;
    pos = frame % cycle_len;
    if (pos < half)
        t = (pos * 256) / half;
    else
        t = 256 - ((pos - half) * 256) / half;
    return clampi(t, 0, 256);
}

static void put_px(uint32_t *fb, uint32_t stride, uint32_t w, uint32_t h,
                   int x, int y, uint32_t c)
{
    if (x < 0 || y < 0 || (uint32_t)x >= w || (uint32_t)y >= h)
        return;
    fb[(uint32_t)y * stride + (uint32_t)x] = c;
}

static int isqrt_u32(uint32_t n)
{
    uint32_t op = n;
    uint32_t res = 0;
    uint32_t one = 1u << 30;

    while (one > op)
        one >>= 2;
    while (one) {
        if (op >= res + one) {
            op -= res + one;
            res = res + (one << 1);
        }
        res >>= 1;
        one >>= 2;
    }
    return (int)res;
}

/*
 * atan(t) for t = i/256, i in 0..256; result in Q16 quarter units (0..16384).
 */
static const uint16_t atan_q16_lut[257] = {
    0, 41, 81, 122, 163, 204, 244, 285, 326, 367, 407, 448, 489, 529, 570, 610,
    651, 692, 732, 773, 813, 854, 894, 935, 975, 1015, 1056, 1096, 1136, 1177, 1217, 1257,
    1297, 1337, 1377, 1417, 1457, 1497, 1537, 1577, 1617, 1656, 1696, 1736, 1775, 1815, 1854, 1894,
    1933, 1973, 2012, 2051, 2090, 2129, 2168, 2207, 2246, 2285, 2324, 2363, 2401, 2440, 2478, 2517,
    2555, 2594, 2632, 2670, 2708, 2746, 2784, 2822, 2860, 2897, 2935, 2973, 3010, 3047, 3085, 3122,
    3159, 3196, 3233, 3270, 3307, 3344, 3380, 3417, 3453, 3490, 3526, 3562, 3599, 3635, 3670, 3706,
    3742, 3778, 3813, 3849, 3884, 3920, 3955, 3990, 4025, 4060, 4095, 4129, 4164, 4199, 4233, 4267,
    4302, 4336, 4370, 4404, 4438, 4471, 4505, 4539, 4572, 4605, 4639, 4672, 4705, 4738, 4771, 4803,
    4836, 4869, 4901, 4933, 4966, 4998, 5030, 5062, 5094, 5125, 5157, 5188, 5220, 5251, 5282, 5313,
    5344, 5375, 5406, 5437, 5467, 5498, 5528, 5559, 5589, 5619, 5649, 5679, 5708, 5738, 5768, 5797,
    5826, 5856, 5885, 5914, 5943, 5972, 6000, 6029, 6058, 6086, 6114, 6142, 6171, 6199, 6227, 6254,
    6282, 6310, 6337, 6365, 6392, 6419, 6446, 6473, 6500, 6527, 6554, 6580, 6607, 6633, 6660, 6686,
    6712, 6738, 6764, 6790, 6815, 6841, 6867, 6892, 6917, 6943, 6968, 6993, 7018, 7043, 7068, 7092,
    7117, 7141, 7166, 7190, 7214, 7238, 7262, 7286, 7310, 7334, 7358, 7381, 7405, 7428, 7451, 7475,
    7498, 7521, 7544, 7566, 7589, 7612, 7635, 7657, 7679, 7702, 7724, 7746, 7768, 7790, 7812, 7834,
    7856, 7877, 7899, 7920, 7942, 7963, 7984, 8005, 8026, 8047, 8068, 8089, 8110, 8131, 8151, 8172,
    8192
};

static int atan_q16_octant(int opposite, int adjacent)
{
    int idx;

    if (adjacent <= 0)
        return (opposite > 0) ? 8192 : 0;
    if (opposite <= 0)
        return 0;
    idx = (opposite << 8) / adjacent;
    if (idx > 256)
        idx = 256;
    return (int)atan_q16_lut[idx];
}

/* Angle 0..65535 clockwise from top. */
static int angle_top_cw(int dx, int dy)
{
    int u = dx;
    int v = -dy;
    int au = u >= 0 ? u : -u;
    int av = v >= 0 ? v : -v;
    int a_q;

    if (u >= 0 && v >= 0) {
        if (av >= au)
            a_q = atan_q16_octant(au, av);
        else
            a_q = ANGLE_QTR - atan_q16_octant(av, au);
    } else if (u >= 0 && v < 0) {
        a_q = ANGLE_QTR + atan_q16_octant(av, au);
    } else if (u < 0 && v < 0) {
        if (av >= au)
            a_q = 2 * ANGLE_QTR + atan_q16_octant(au, av);
        else
            a_q = 3 * ANGLE_QTR - atan_q16_octant(av, au);
    } else {
        a_q = 3 * ANGLE_QTR + atan_q16_octant(av, au);
    }
    return a_q & (ANGLE_FULL - 1);
}

/*
 * sin/cos for angle CW from top (Q16 angle, Q15 trig).
 * x = sin(a)*r, y = -cos(a)*r.
 */
static const int16_t sin_q15[257] = {
    0, 402, 804, 1206, 1608, 2009, 2410, 2811, 3212, 3612, 4011, 4410, 4808, 5205, 5602, 5998,
    6393, 6786, 7179, 7571, 7962, 8351, 8739, 9126, 9512, 9896, 10278, 10659, 11039, 11417, 11793, 12167,
    12539, 12910, 13279, 13645, 14010, 14372, 14732, 15090, 15446, 15800, 16151, 16499, 16846, 17189, 17530, 17869,
    18204, 18537, 18868, 19195, 19519, 19841, 20159, 20475, 20787, 21096, 21403, 21706, 22005, 22301, 22594, 22884,
    23170, 23452, 23731, 24007, 24279, 24547, 24811, 25072, 25329, 25582, 25832, 26077, 26319, 26556, 26790, 27019,
    27245, 27466, 27683, 27896, 28105, 28310, 28510, 28706, 28898, 29085, 29268, 29447, 29621, 29791, 29956, 30117,
    30273, 30424, 30571, 30714, 30852, 30985, 31113, 31237, 31356, 31470, 31580, 31685, 31785, 31880, 31971, 32057,
    32137, 32213, 32285, 32351, 32412, 32469, 32521, 32567, 32609, 32646, 32678, 32705, 32728, 32745, 32757, 32765,
    32767
};

static void ang_to_ring_xy(int ang, int *ox, int *oy)
{
    int a = ang & (ANGLE_FULL - 1);
    int quad = a / ANGLE_QTR;
    int rem = a % ANGLE_QTR;
    int mirror = ANGLE_QTR - rem;
    int sx;
    int sy;

    switch (quad) {
    case 0:
        sx = sin_q15[rem >> 8];
        sy = sin_q15[mirror >> 8];
        break;
    case 1:
        sx = sin_q15[mirror >> 8];
        sy = -sin_q15[rem >> 8];
        break;
    case 2:
        sx = -sin_q15[rem >> 8];
        sy = -sin_q15[mirror >> 8];
        break;
    default:
        sx = -sin_q15[mirror >> 8];
        sy = sin_q15[rem >> 8];
        break;
    }
    *ox = (sx * RING_R) >> 15;
    *oy = (sy * RING_R) >> 15;
}

static int ring_coverage_q8(int dist)
{
    int cov_in;
    int cov_out;

    cov_in = clampi(((dist - INNER_R + 1) * AA_Q8 + 1) / 2, 0, AA_Q8);
    cov_out = clampi(((OUTER_R - dist + 1) * AA_Q8 + 1) / 2, 0, AA_Q8);
    return (cov_in * cov_out) / AA_Q8;
}

static int disc_coverage_q8(int dist, int r)
{
    return clampi((r + 1 - dist) * 128, 0, AA_Q8);
}

static void blend_px(uint32_t *fb, uint32_t stride, uint32_t w, uint32_t h,
                     int x, int y, uint32_t src, int alpha_q8)
{
    uint32_t *dstp;
    uint32_t dst;
    int sr;
    int sg;
    int sb;
    int dr;
    int dg;
    int db;
    uint32_t out;

    if (alpha_q8 <= 0)
        return;
    if (x < 0 || y < 0 || (uint32_t)x >= w || (uint32_t)y >= h)
        return;

    dstp = &fb[(uint32_t)y * stride + (uint32_t)x];
    if (alpha_q8 >= AA_Q8) {
        *dstp = src;
        return;
    }

    dst = *dstp;
    sr = (int)((src >> 16) & 0xFFu);
    sg = (int)((src >> 8) & 0xFFu);
    sb = (int)(src & 0xFFu);
    dr = (int)((dst >> 16) & 0xFFu);
    dg = (int)((dst >> 8) & 0xFFu);
    db = (int)(dst & 0xFFu);
    out = 0xFF000000u |
          (uint32_t)((dr + (sr - dr) * alpha_q8 / AA_Q8) << 16) |
          (uint32_t)((dg + (sg - dg) * alpha_q8 / AA_Q8) << 8) |
          (uint32_t)(db + (sb - db) * alpha_q8 / AA_Q8);
    *dstp = out;
}

static int fill_angular_cov_q8(int ang, int head_ang, int arc_ang)
{
    int d_head;
    int d_tail;
    int tail_ang;
    int cov;

    if (arc_ang <= 0)
        return 0;

    d_head = (head_ang - ang) & (ANGLE_FULL - 1);
    if (d_head > arc_ang)
        return 0;

    tail_ang = (head_ang - arc_ang) & (ANGLE_FULL - 1);
    d_tail = (ang - tail_ang) & (ANGLE_FULL - 1);

    cov = AA_Q8;
    if (d_tail < FEATHER_ANG && arc_ang > FEATHER_ANG)
        cov = (d_tail * AA_Q8) / FEATHER_ANG;
    return clampi(cov, 0, AA_Q8);
}

static void draw_ring(uint32_t *fb, uint32_t stride, uint32_t w, uint32_t h,
                      int cx, int cy, int pct, int rot_ang)
{
    int pad = TIP_R + 2;
    int y0 = cy - RING_R - pad;
    int y1 = cy + RING_R + pad;
    int x0 = cx - RING_R - pad;
    int x1 = cx + RING_R + pad;
    int arc_ang;
    int head_ang;
    int head_x;
    int head_y;
    int tail_x;
    int tail_y;
    int y;
    int x;

    pct = clampi(pct, 0, 100);
    head_ang = rot_ang & (ANGLE_FULL - 1);
    arc_ang = (pct * ANGLE_FULL) / 100;
    if (pct > 0 && arc_ang < MIN_ARC_ANG)
        arc_ang = MIN_ARC_ANG;

    ang_to_ring_xy(head_ang, &head_x, &head_y);
    head_x += cx;
    head_y += cy;
    ang_to_ring_xy((head_ang - arc_ang) & (ANGLE_FULL - 1), &tail_x, &tail_y);
    tail_x += cx;
    tail_y += cy;

    for (y = y0; y <= y1; y++) {
        for (x = x0; x <= x1; x++) {
            int dx = x - cx;
            int dy = y - cy;
            int dist_sq = dx * dx + dy * dy;
            int dist = isqrt_u32((uint32_t)dist_sq);
            int ring_cov;
            int fill_cov;
            int ang;
            int cap_head;
            int cap_tail;
            int glow_cov;

            ring_cov = ring_coverage_q8(dist);
            if (ring_cov <= 0)
                continue;

            blend_px(fb, stride, w, h, x, y, SPLASH_TRACK, ring_cov);

            if (pct <= 0)
                continue;

            ang = angle_top_cw(dx, dy);
            fill_cov = (ring_cov * fill_angular_cov_q8(ang, head_ang, arc_ang)) / AA_Q8;

            cap_head = disc_coverage_q8(isqrt_u32((uint32_t)((x - head_x) * (x - head_x) +
                                                             (y - head_y) * (y - head_y))), TIP_R);
            cap_tail = disc_coverage_q8(isqrt_u32((uint32_t)((x - tail_x) * (x - tail_x) +
                                                             (y - tail_y) * (y - tail_y))), TIP_R);
            if (cap_head > fill_cov)
                fill_cov = cap_head;
            if (cap_tail > fill_cov)
                fill_cov = cap_tail;

            if (fill_cov > 0)
                blend_px(fb, stride, w, h, x, y, SPLASH_FILL, fill_cov);

            glow_cov = disc_coverage_q8(isqrt_u32((uint32_t)((x - head_x) * (x - head_x) +
                                                             (y - head_y) * (y - head_y))), TIP_R - 1);
            if (glow_cov > 0)
                blend_px(fb, stride, w, h, x, y, SPLASH_GLOW, glow_cov / 3);
        }
    }
}

static void splash_delay(void)
{
    volatile uint32_t n = 208000u;

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
    clear_r = RING_R + TIP_R + 4;

    for (frame = 0; frame <= SPLASH_FRAMES; frame++) {
        int t_raw = pingpong_q8(frame, FILL_CYCLE);
        int t_eased = ease_in_out_q8(t_raw);
        int pct = lerpi(0, 100, t_eased);
        int rot_ang = (frame * ROT_SPEED) % ANGLE_FULL;
        int y, x;

        for (y = cy - clear_r; y <= cy + clear_r; y++) {
            for (x = cx - clear_r; x <= cx + clear_r; x++)
                put_px(fb, mode.width, mode.width, mode.height, x, y, SPLASH_BG);
        }

        draw_ring(fb, mode.width, mode.width, mode.height, cx, cy, pct, rot_ang);
        (void)ops->present(fb, mode.width);
        splash_delay();
    }

    kfree(fb);
    klog("[boot] splash done\n");
}
