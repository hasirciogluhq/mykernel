#ifndef MYKERNEL_GFX_ACCEL_H
#define MYKERNEL_GFX_ACCEL_H

#include <gfx/surface.h>

/*
 * "Hardware-accelerated" paths — kernel-side bulk pixel ops.
 * On BGA/QEMU this is optimized dword fill/blit (LFB-class throughput).
 * Rounded fills run in the same accel path.
 */
void gx_accel_fill(gx_surface *s, gx_rect r, gx_color c);
void gx_accel_fill_round(gx_surface *s, gx_rect r, int32_t radius, gx_color c);
void gx_accel_blit(gx_surface *dst, int32_t dx, int32_t dy, const gx_surface *src);
void gx_accel_blit_rect(gx_surface *dst, int32_t dx, int32_t dy,
                        const gx_surface *src, gx_rect src_r);

/* True if (lx,ly) is inside a w×h rounded rect with corner radius r (local coords). */
int gx_point_in_round_rect(int32_t lx, int32_t ly, int32_t w, int32_t h, int32_t r);

#endif
