#include <drivers/keyboard.h>
#include <drivers/console.h>
#include <arch/x86/io.h>

#define KBD_DATA 0x60
#define KBD_STAT 0x64

static int shift_on;
static int caps_on;

static const char keymap[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n', 0,
    'a','s','d','f','g','h','j','k','l',';','\'','`', 0, '\\',
    'z','x','c','v','b','n','m',',','.','/', 0, '*', 0, ' ',
};

static const char keymap_shift[128] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n', 0,
    'A','S','D','F','G','H','J','K','L',':','"','~', 0, '|',
    'Z','X','C','V','B','N','M','<','>','?', 0, '*', 0, ' ',
};

void keyboard_init(void)
{
    shift_on = 0;
    caps_on = 0;

    /* mask PIC — we poll the keyboard, no IRQ handlers yet */
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}

void keyboard_poll(void)
{
    while (inb(KBD_STAT) & 1) {
        uint8_t sc = inb(KBD_DATA);

        if (sc == 0x2A || sc == 0x36) {
            shift_on = 1;
            continue;
        }
        if (sc == 0xAA || sc == 0xB6) {
            shift_on = 0;
            continue;
        }
        if (sc == 0x3A) {
            caps_on = !caps_on;
            continue;
        }

        if (sc & 0x80)
            continue; /* break code */

        if (sc >= 128)
            continue;

        char c = shift_on ? keymap_shift[sc] : keymap[sc];
        if (!c)
            continue;

        if (caps_on && c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        else if (caps_on && c >= 'A' && c <= 'Z' && !shift_on)
            c = (char)(c - 'A' + 'a');

        console_push_scancode_char(c);
    }
}

int keyboard_has_input(void)
{
    keyboard_poll();
    return 0; /* console owns the queue; shell checks console */
}
