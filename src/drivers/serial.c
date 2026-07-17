#include <drivers/serial.h>
#include <arch/x86/io.h>
#include <kernel/spinlock.h>

#define COM1 0x3F8

static int g_serial_ready;
static spinlock_t g_serial_lock;

void serial_init(void)
{
    /* 115200 8N1, FIFO on — QEMU accepts this immediately. */
    outb(COM1 + 1, 0x00); /* disable IRQs */
    outb(COM1 + 3, 0x80); /* DLAB on */
    outb(COM1 + 0, 0x01); /* divisor lo (115200) */
    outb(COM1 + 1, 0x00); /* divisor hi */
    outb(COM1 + 3, 0x03); /* 8N1, DLAB off */
    outb(COM1 + 2, 0xC7); /* FIFO enable/clear */
    outb(COM1 + 4, 0x0B); /* IRQs enabled, RTS/DSR set */
    spin_init(&g_serial_lock);
    g_serial_ready = 1;

    serial_print("\n[klog] serial COM1 ready\n");
}

static int serial_tx_empty(void)
{
    return inb(COM1 + 5) & 0x20;
}

static void serial_putc_unlocked(char c)
{
    if (c == '\n') {
        while (!serial_tx_empty())
            ;
        outb(COM1, (uint8_t)'\r');
    }
    while (!serial_tx_empty())
        ;
    outb(COM1, (uint8_t)c);
}

void serial_putc(char c)
{
    uint32_t flags;
    if (!g_serial_ready)
        return;
    flags = spin_lock_irqsave(&g_serial_lock);
    serial_putc_unlocked(c);
    spin_unlock_irqrestore(&g_serial_lock, flags);
}

void serial_write(const char *s, size_t n)
{
    uint32_t flags;
    size_t i;
    if (!s || !g_serial_ready)
        return;
    flags = spin_lock_irqsave(&g_serial_lock);
    for (i = 0; i < n; i++)
        serial_putc_unlocked(s[i]);
    spin_unlock_irqrestore(&g_serial_lock, flags);
}

void serial_print(const char *s)
{
    uint32_t flags;
    if (!s || !g_serial_ready)
        return;
    flags = spin_lock_irqsave(&g_serial_lock);
    while (*s)
        serial_putc_unlocked(*s++);
    spin_unlock_irqrestore(&g_serial_lock, flags);
}

void serial_print_uint(uint32_t n)
{
    char buf[11];
    int i = 0;
    uint32_t flags;

    if (!g_serial_ready)
        return;

    flags = spin_lock_irqsave(&g_serial_lock);
    if (n == 0) {
        serial_putc_unlocked('0');
        spin_unlock_irqrestore(&g_serial_lock, flags);
        return;
    }
    while (n > 0 && i < 10) {
        buf[i++] = (char)('0' + (n % 10));
        n /= 10;
    }
    while (i > 0)
        serial_putc_unlocked(buf[--i]);
    spin_unlock_irqrestore(&g_serial_lock, flags);
}

void serial_print_hex(uint32_t n)
{
    static const char hex[] = "0123456789abcdef";
    uint32_t flags;
    int i;

    if (!g_serial_ready)
        return;

    flags = spin_lock_irqsave(&g_serial_lock);
    serial_putc_unlocked('0');
    serial_putc_unlocked('x');
    for (i = 7; i >= 0; i--)
        serial_putc_unlocked(hex[(n >> (i * 4)) & 0xF]);
    spin_unlock_irqrestore(&g_serial_lock, flags);
}

void klog(const char *s)
{
    serial_print(s);
}

void klog_uint(const char *prefix, uint32_t n)
{
    uint32_t flags;
    if (!g_serial_ready)
        return;
    flags = spin_lock_irqsave(&g_serial_lock);
    if (prefix) {
        while (*prefix)
            serial_putc_unlocked(*prefix++);
    }
    {
        char buf[11];
        int i = 0;
        uint32_t v = n;
        if (v == 0) {
            serial_putc_unlocked('0');
        } else {
            while (v > 0 && i < 10) {
                buf[i++] = (char)('0' + (v % 10));
                v /= 10;
            }
            while (i > 0)
                serial_putc_unlocked(buf[--i]);
        }
    }
    serial_putc_unlocked('\n');
    spin_unlock_irqrestore(&g_serial_lock, flags);
}

void klog_hex(const char *prefix, uint32_t n)
{
    static const char hex[] = "0123456789abcdef";
    uint32_t flags;
    int i;

    if (!g_serial_ready)
        return;
    flags = spin_lock_irqsave(&g_serial_lock);
    if (prefix) {
        while (*prefix)
            serial_putc_unlocked(*prefix++);
    }
    serial_putc_unlocked('0');
    serial_putc_unlocked('x');
    for (i = 7; i >= 0; i--)
        serial_putc_unlocked(hex[(n >> (i * 4)) & 0xF]);
    serial_putc_unlocked('\n');
    spin_unlock_irqrestore(&g_serial_lock, flags);
}
