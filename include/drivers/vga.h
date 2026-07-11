#ifndef MYKERNEL_VGA_H
#define MYKERNEL_VGA_H

#include <kernel/types.h>

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

#define VGA_COLOR_BLACK         0x0
#define VGA_COLOR_BLUE          0x1
#define VGA_COLOR_GREEN         0x2
#define VGA_COLOR_CYAN          0x3
#define VGA_COLOR_RED           0x4
#define VGA_COLOR_MAGENTA       0x5
#define VGA_COLOR_BROWN         0x6
#define VGA_COLOR_LIGHT_GREY    0x7
#define VGA_COLOR_DARK_GREY     0x8
#define VGA_COLOR_LIGHT_BLUE    0x9
#define VGA_COLOR_LIGHT_GREEN   0xA
#define VGA_COLOR_LIGHT_CYAN    0xB
#define VGA_COLOR_LIGHT_RED     0xC
#define VGA_COLOR_LIGHT_MAGENTA 0xD
#define VGA_COLOR_YELLOW        0xE
#define VGA_COLOR_WHITE         0xF

#define VGA_ATTR(fg, bg) ((uint8_t)(((bg) << 4) | ((fg) & 0x0F)))

void vga_init(void);
void vga_clear(uint8_t attr);
void vga_set_color(uint8_t attr);
void vga_putc(char c);
void vga_write(const char *s, size_t n);
void vga_print(const char *s);
void vga_print_uint(uint32_t n);
void vga_print_at(unsigned row, unsigned col, const char *s, uint8_t attr);

#endif
