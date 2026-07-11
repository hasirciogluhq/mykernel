#include <drivers/ps2.h>
#include <drivers/keyboard.h>
#include <drivers/mouse.h>
#include <arch/x86/io.h>

static int wait_write(void)
{
    for (int i = 0; i < 100000; i++) {
        if (!(inb(PS2_STAT) & PS2_STAT_IBF))
            return 0;
    }
    return -1;
}

static int wait_read(void)
{
    for (int i = 0; i < 100000; i++) {
        if (inb(PS2_STAT) & PS2_STAT_OBF)
            return 0;
    }
    return -1;
}

int ps2_wait_write(void) { return wait_write(); }
int ps2_wait_read(void)  { return wait_read(); }

void ps2_write_cmd(uint8_t cmd)
{
    wait_write();
    outb(PS2_STAT, cmd);
}

void ps2_write_data(uint8_t data)
{
    wait_write();
    outb(PS2_DATA, data);
}

uint8_t ps2_read_data(void)
{
    wait_read();
    return inb(PS2_DATA);
}

int ps2_write_mouse(uint8_t data)
{
    ps2_write_cmd(0xD4);
    ps2_write_data(data);
    if (wait_read() < 0)
        return -1;
    return (int)inb(PS2_DATA);
}

void ps2_init(void)
{
    /* disable devices while configuring */
    ps2_write_cmd(0xAD); /* disable kbd */
    ps2_write_cmd(0xA7); /* disable mouse */

    /* flush output buffer */
    while (inb(PS2_STAT) & PS2_STAT_OBF)
        (void)inb(PS2_DATA);

    /* read controller config, enable clocks, disable IRQs (we poll) */
    ps2_write_cmd(0x20);
    uint8_t cfg = ps2_read_data();
    cfg &= ~0x03; /* clear IRQ1 / IRQ12 */
    cfg |= 0x01;  /* translation ok for kbd */
    cfg &= ~0x20; /* enable mouse clock */
    cfg &= ~0x10; /* enable kbd clock */
    ps2_write_cmd(0x60);
    ps2_write_data(cfg);

    ps2_write_cmd(0xAE); /* enable kbd */
    ps2_write_cmd(0xA8); /* enable aux */

    /* mask PIC — polling only */
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}

void ps2_poll(void)
{
    for (;;) {
        uint8_t st = inb(PS2_STAT);
        if (!(st & PS2_STAT_OBF))
            break;
        /* AUX bit must be sampled before consuming DATA */
        uint8_t data = inb(PS2_DATA);
        if (st & PS2_STAT_AUX)
            mouse_handle_byte(data);
        else
            keyboard_handle_scancode(data);
    }
}
