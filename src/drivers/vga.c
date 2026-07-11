#include <drivers/vga.h>

static volatile uint16_t *const VGA = (volatile uint16_t *)0xB8000;
static unsigned cursor_row;
static unsigned cursor_col;
static uint8_t  text_attr = VGA_ATTR(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

static void scroll_if_needed(void)
{
    if (cursor_row < VGA_HEIGHT)
        return;

    for (unsigned r = 1; r < VGA_HEIGHT; r++) {
        for (unsigned c = 0; c < VGA_WIDTH; c++)
            VGA[(r - 1) * VGA_WIDTH + c] = VGA[r * VGA_WIDTH + c];
    }

    uint16_t blank = (uint16_t)(' ' | (text_attr << 8));
    for (unsigned c = 0; c < VGA_WIDTH; c++)
        VGA[(VGA_HEIGHT - 1) * VGA_WIDTH + c] = blank;

    cursor_row = VGA_HEIGHT - 1;
}

void vga_init(void)
{
    cursor_row = 0;
    cursor_col = 0;
    text_attr = VGA_ATTR(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_clear(text_attr);
}

void vga_clear(uint8_t attr)
{
    uint16_t blank = (uint16_t)(' ' | (attr << 8));
    for (unsigned i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        VGA[i] = blank;
    cursor_row = 0;
    cursor_col = 0;
    text_attr = attr;
}

void vga_set_color(uint8_t attr)
{
    text_attr = attr;
}

void vga_putc(char c)
{
    if (c == '\n') {
        cursor_col = 0;
        cursor_row++;
        scroll_if_needed();
        return;
    }

    if (c == '\r') {
        cursor_col = 0;
        return;
    }

    if (c == '\t') {
        unsigned next = (cursor_col + 4) & ~(unsigned)3;
        while (cursor_col < next)
            vga_putc(' ');
        return;
    }

    VGA[cursor_row * VGA_WIDTH + cursor_col] =
        (uint16_t)((uint8_t)c | (text_attr << 8));

    if (++cursor_col >= VGA_WIDTH) {
        cursor_col = 0;
        cursor_row++;
        scroll_if_needed();
    }
}

void vga_write(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        vga_putc(s[i]);
}

void vga_print(const char *s)
{
    while (*s)
        vga_putc(*s++);
}

void vga_print_uint(uint32_t n)
{
    char buf[11];
    int i = 0;

    if (n == 0) {
        vga_putc('0');
        return;
    }

    while (n > 0 && i < 10) {
        buf[i++] = (char)('0' + (n % 10));
        n /= 10;
    }
    while (i > 0)
        vga_putc(buf[--i]);
}

void vga_print_at(unsigned row, unsigned col, const char *s, uint8_t attr)
{
    while (*s && col < VGA_WIDTH && row < VGA_HEIGHT) {
        VGA[row * VGA_WIDTH + col] = (uint16_t)((uint8_t)*s | (attr << 8));
        s++;
        col++;
    }
}
