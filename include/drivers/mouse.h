#ifndef MYKERNEL_DRIVERS_MOUSE_H
#define MYKERNEL_DRIVERS_MOUSE_H

#include <kernel/types.h>

#define MOUSE_BTN_LEFT   0x01
#define MOUSE_BTN_RIGHT  0x02
#define MOUSE_BTN_MIDDLE 0x04

typedef struct mouse_state {
    int32_t x;
    int32_t y;
    int32_t dx;
    int32_t dy;
    uint8_t buttons;      /* current */
    uint8_t buttons_prev;
    int     ready;
} mouse_state_t;

typedef enum {
    MOUSE_EV_NONE = 0,
    MOUSE_EV_MOVE,
    MOUSE_EV_DOWN,
    MOUSE_EV_UP
} mouse_ev_type;

typedef struct mouse_event {
    mouse_ev_type type;
    int32_t       x;
    int32_t       y;
    uint8_t       button; /* which button changed for down/up */
    uint8_t       buttons;
} mouse_event_t;

void               mouse_init(void);
void               mouse_handle_byte(uint8_t b);
void               mouse_set_bounds(int32_t w, int32_t h);
const mouse_state_t *mouse_get(void);
int                mouse_pop_event(mouse_event_t *ev); /* 1 = event, 0 = empty */

#endif
