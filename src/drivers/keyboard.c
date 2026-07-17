#include <drivers/keyboard.h>
#include <drivers/ps2.h>

#define KBD_QSIZE 128

static int shift_on;
static int caps_on;
static int ctrl_on;
static int alt_on;
static int altgr_on;
static int super_on;
static int menu_on;
static int e0_prefix;

static uint8_t q[KBD_QSIZE];
static unsigned q_head;
static unsigned q_tail;

/* Turkish Q (ISO-8859-9) — physical scancode set 1 positions */
static const uint8_t keymap[128] = {
    /* 00 */ 0,    27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '*', '-', '\b',
    /* 0F */ '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', TR_idotless, 'o', 'p', TR_gbreve, TR_udiaer, '\n', 0,
    /* 1E */ 'a',  's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', TR_scedil, 'i', '"', 0, ',',
    /* 2C */ 'z',  'x', 'c', 'v', 'b', 'n', 'm', TR_odiaer, TR_ccedil, '.', 0, '*', 0, ' ',
};

static const uint8_t keymap_shift[128] = {
    /* 00 */ 0,    27,  '!', '\'', '^', '+', '%', '&', '/', '(', ')', '=', '?', '_', '\b',
    /* 0F */ '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', TR_GBREVE, TR_UDIAER, '\n', 0,
    /* 1E */ 'A',  'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', TR_SCEDIL, TR_IDOT, '\'', 0, ';',
    /* 2C */ 'Z',  'X', 'C', 'V', 'B', 'N', 'M', TR_ODIAER, TR_CCEDIL, ':', 0, '*', 0, ' ',
};

/* AltGr layer for common symbols on TRQ */
static const uint8_t keymap_altgr[128] = {
    /* sparse — only filled where needed */
    [0x02] = '!',  /* 1 */
    [0x03] = '\'', /* 2 → £ often; keep ' */
    [0x04] = '#',  /* 3 */
    [0x05] = '$',  /* 4 */
    [0x06] = '%',
    [0x07] = '&',
    [0x08] = '{',  /* 7 */
    [0x09] = '[',  /* 8 */
    [0x0A] = ']',  /* 9 */
    [0x0B] = '}',  /* 0 */
    [0x0C] = '\\', /* * */
    [0x0D] = '|',  /* - */
    [0x10] = '@',  /* q */
    [0x11] = 0,    /* w */
    [0x12] = 0x80, /* € placeholder — skip */
    [0x1A] = 0xA8, /* ¨ */
    [0x1B] = '~',
    [0x1E] = 0xE6, /* æ */
    [0x1F] = 0xDF, /* ß */
    [0x2B] = '`',
    [0x35] = '>',  /* . → > (shell redirect; TR Shift+. is :) */
    [0x56] = '|',
};

static void q_push(uint8_t c)
{
    unsigned next = (q_head + 1) % KBD_QSIZE;
    if (next == q_tail)
        return;
    q[q_head] = c;
    q_head = next;
}

static uint8_t turkish_toggle_case(uint8_t c)
{
    if (c >= 'a' && c <= 'z')
        return (uint8_t)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z')
        return (uint8_t)(c - 'A' + 'a');

    switch (c) {
    case TR_idotless: return 'I';
    case 'I':         return TR_idotless;
    case 'i':         return TR_IDOT;
    case TR_IDOT:     return 'i';
    case TR_gbreve:   return TR_GBREVE;
    case TR_GBREVE:   return TR_gbreve;
    case TR_udiaer:   return TR_UDIAER;
    case TR_UDIAER:   return TR_udiaer;
    case TR_scedil:   return TR_SCEDIL;
    case TR_SCEDIL:   return TR_scedil;
    case TR_odiaer:   return TR_ODIAER;
    case TR_ODIAER:   return TR_odiaer;
    case TR_ccedil:   return TR_CCEDIL;
    case TR_CCEDIL:   return TR_ccedil;
    default:          return c;
    }
}

static int is_cased_letter(uint8_t c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
        return 1;
    switch (c) {
    case TR_idotless: case 'i': case TR_IDOT: case 'I':
    case TR_gbreve: case TR_GBREVE:
    case TR_udiaer: case TR_UDIAER:
    case TR_scedil: case TR_SCEDIL:
    case TR_odiaer: case TR_ODIAER:
    case TR_ccedil: case TR_CCEDIL:
        return 1;
    default:
        return 0;
    }
}

void keyboard_init(void)
{
    shift_on = 0;
    caps_on = 0;
    ctrl_on = 0;
    alt_on = 0;
    altgr_on = 0;
    super_on = 0;
    menu_on = 0;
    e0_prefix = 0;
    q_head = 0;
    q_tail = 0;
}

uint8_t keyboard_modifiers(void)
{
    uint8_t m = 0;
    if (shift_on) m |= KBD_MOD_SHIFT;
    if (ctrl_on)  m |= KBD_MOD_CTRL;
    if (alt_on)   m |= KBD_MOD_ALT;
    if (altgr_on) m |= KBD_MOD_ALTGR;
    if (caps_on)  m |= KBD_MOD_CAPS;
    if (super_on) m |= KBD_MOD_SUPER;
    if (menu_on)  m |= KBD_MOD_MENU;
    return m;
}

void keyboard_handle_scancode(uint8_t sc)
{
    if (sc == 0xE0) {
        e0_prefix = 1;
        return;
    }

    int release = (sc & 0x80) != 0;
    uint8_t code = sc & 0x7F;
    int ext = e0_prefix;
    e0_prefix = 0;

    /* modifiers */
    if (code == 0x2A || code == 0x36) {
        shift_on = !release;
        return;
    }
    if (code == 0x1D) {
        ctrl_on = !release;
        return;
    }
    if (code == 0x38) {
        if (ext)
            altgr_on = !release;
        else
            alt_on = !release;
        return;
    }
    if (code == 0x3A && !release && !ext) {
        caps_on = !caps_on;
        return;
    }
    /* E0 Left/Right GUI (Win/Cmd) and Menu / App key */
    if (ext && (code == 0x5B || code == 0x5C)) {
        super_on = !release;
        return;
    }
    if (ext && code == 0x5D) {
        menu_on = !release;
        return;
    }

    if (release)
        return;

    /* ISO 102nd key (< > |) */
    if (code == 0x56) {
        uint8_t c = shift_on ? '>' : '<';
        if (altgr_on)
            c = '|';
        q_push(c);
        return;
    }

    if (code >= 128)
        return;

    uint8_t c = 0;
    if (altgr_on && keymap_altgr[code]) {
        c = keymap_altgr[code];
        if (c == 0x80)
            return; /* unsupported */
    } else if (shift_on) {
        c = keymap_shift[code];
    } else {
        c = keymap[code];
    }

    if (!c)
        return;

    /* Caps Lock toggles letter case (TR-aware: ı/I, i/İ) */
    if (caps_on && is_cased_letter(c) && !altgr_on)
        c = turkish_toggle_case(c);

    if (ctrl_on && c >= 'a' && c <= 'z')
        c = (uint8_t)(c - 'a' + 1);
    else if (ctrl_on && c >= 'A' && c <= 'Z')
        c = (uint8_t)(c - 'A' + 1);

    q_push(c);
}

void keyboard_poll(void)
{
    ps2_poll();
}

int keyboard_has_char(void)
{
    keyboard_poll();
    return q_head != q_tail;
}

int keyboard_getchar(void)
{
    keyboard_poll();
    if (q_head == q_tail)
        return -1;
    uint8_t c = q[q_tail];
    q_tail = (q_tail + 1) % KBD_QSIZE;
    return (int)c;
}

/* legacy name used elsewhere */
int keyboard_has_input(void)
{
    return keyboard_has_char();
}
