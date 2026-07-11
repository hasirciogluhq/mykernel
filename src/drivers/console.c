#include <drivers/console.h>
#include <drivers/keyboard.h>
#include <drivers/vga.h>
#include <kernel/scheduler.h>

void console_init(void)
{
}

void console_push_scancode_char(char c)
{
    (void)c; /* legacy; keyboard owns the queue now */
}

void console_putc(char c)
{
    vga_putc(c);
}

void console_write(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        console_putc(s[i]);
}

void console_print(const char *s)
{
    while (*s)
        console_putc(*s++);
}

int console_getc(void)
{
    for (;;) {
        int c = keyboard_getchar();
        if (c >= 0)
            return c;
        schedule();
    }
}

ssize_t console_read(void *buf, size_t count)
{
    if (count == 0)
        return 0;

    char *out = buf;
    size_t n = 0;

    while (n < count) {
        int c = console_getc();
        if (c == '\b') {
            if (n > 0) {
                n--;
                console_write("\b \b", 3);
            }
            continue;
        }
        out[n++] = (char)c;
        console_putc((char)c);
        if (c == '\n')
            break;
    }
    return (ssize_t)n;
}
