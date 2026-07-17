#ifndef MYKERNEL_DRIVERS_MOUSE_H
#define MYKERNEL_DRIVERS_MOUSE_H

#include <kernel/types.h>

#define MOUSE_BTN_LEFT    0x01
#define MOUSE_BTN_RIGHT   0x02
#define MOUSE_BTN_MIDDLE  0x04
#define MOUSE_BTN_BACK    0x08  /* side / "Browser Back" / X1 */
#define MOUSE_BTN_FORWARD 0x10  /* side / "Browser Forward" / X2 */

typedef struct mouse_state {
    int32_t x;
    int32_t y;
    int32_t dx;
    int32_t dy;
    int32_t wheel;        /* accumulated notches since last consume */
    uint8_t buttons;      /* current (5-button mask) */
    uint8_t buttons_prev;
    uint8_t has_wheel;    /* IntelliMouse / Explorer protocol armed */
    uint8_t has_side_btns;
    int     ready;
} mouse_state_t;

typedef enum {
    MOUSE_EV_NONE = 0,
    MOUSE_EV_MOVE,
    MOUSE_EV_DOWN,
    MOUSE_EV_UP,
    MOUSE_EV_WHEEL
} mouse_ev_type;

typedef struct mouse_event {
    mouse_ev_type type;
    int32_t       x;
    int32_t       y;
    int32_t       wheel;  /* delta for WHEEL events */
    uint8_t       button; /* which button changed for down/up */
    uint8_t       buttons;
} mouse_event_t;

void               mouse_init(void);
void               mouse_handle_byte(uint8_t b);
void               mouse_set_bounds(int32_t w, int32_t h);
const mouse_state_t *mouse_get(void);
/* Return accumulated wheel notches and clear accumulator. */
int32_t            mouse_consume_wheel(void);
int                mouse_pop_event(mouse_event_t *ev); /* 1 = event, 0 = empty */

#endif
