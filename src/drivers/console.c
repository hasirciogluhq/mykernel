#include <drivers/console.h>
#include <drivers/keyboard.h>
#include <drivers/vga.h>
#include <kernel/scheduler.h>

#define CONSOLE_RX_SIZE 256

static char     rx_buf[CONSOLE_RX_SIZE];
static unsigned rx_head;
static unsigned rx_tail;

void console_init(void)
{
    rx_head = 0;
    rx_tail = 0;
}

void console_push_scancode_char(char c)
{
    unsigned next = (rx_head + 1) % CONSOLE_RX_SIZE;
    if (next == rx_tail)
        return; /* drop on overflow */
    rx_buf[rx_head] = c;
    rx_head = next;
}

static int rx_pop(void)
{
    if (rx_tail == rx_head)
        return -1;
    char c = rx_buf[rx_tail];
    rx_tail = (rx_tail + 1) % CONSOLE_RX_SIZE;
    return (unsigned char)c;
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
        keyboard_poll();
        int c = rx_pop();
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

    /* line-buffered: wait for at least one char; stop at \n or count */
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
