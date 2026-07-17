#ifndef MYKERNEL_DRIVERS_KEYBOARD_H
#define MYKERNEL_DRIVERS_KEYBOARD_H

#include <kernel/types.h>

/* ISO-8859-9 (Latin-5) Turkish letters produced by TRQ layout */
#define TR_GBREVE   0xD0  /* Ğ */
#define TR_gbreve   0xF0  /* ğ */
#define TR_UDIAER   0xDC  /* Ü */
#define TR_udiaer   0xFC  /* ü */
#define TR_SCEDIL   0xDE  /* Ş */
#define TR_scedil   0xFE  /* ş */
#define TR_IDOT     0xDD  /* İ */
#define TR_idotless 0xFD  /* ı */
#define TR_ODIAER   0xD6  /* Ö */
#define TR_odiaer   0xF6  /* ö */
#define TR_CCEDIL   0xC7  /* Ç */
#define TR_ccedil   0xE7  /* ç */

#define KBD_MOD_SHIFT  0x01
#define KBD_MOD_CTRL   0x02
#define KBD_MOD_ALT    0x04
#define KBD_MOD_ALTGR  0x08
#define KBD_MOD_CAPS   0x10
#define KBD_MOD_SUPER  0x20  /* Win / Cmd (left or right) */
#define KBD_MOD_MENU   0x40  /* Application / Menu key */

void    keyboard_init(void);
void    keyboard_handle_scancode(uint8_t sc);
void    keyboard_poll(void);           /* ps2_poll + drain */
int     keyboard_getchar(void);        /* -1 empty, else 0..255 Latin-5 */
int     keyboard_has_char(void);
uint8_t keyboard_modifiers(void);

#endif
