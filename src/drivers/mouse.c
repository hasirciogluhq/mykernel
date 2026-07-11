#include <drivers/mouse.h>
#include <drivers/ps2.h>

#define MOUSE_EVQ 64

static mouse_state_t state;
static int32_t bound_w = 1024;
static int32_t bound_h = 768;

static uint8_t pkt[3];
static int     pkt_i;
static int     synced;

static mouse_event_t evq[MOUSE_EVQ];
static unsigned ev_h;
static unsigned ev_t;

static void ev_push(mouse_event_t *e)
{
    unsigned next = (ev_h + 1) % MOUSE_EVQ;
    if (next == ev_t)
        return;
    evq[ev_h] = *e;
    ev_h = next;
}

static void clamp_pos(void)
{
    if (state.x < 0)
        state.x = 0;
    if (state.y < 0)
        state.y = 0;
    if (state.x >= bound_w)
        state.x = bound_w - 1;
    if (state.y >= bound_h)
        state.y = bound_h - 1;
}

void mouse_set_bounds(int32_t w, int32_t h)
{
    if (w > 0)
        bound_w = w;
    if (h > 0)
        bound_h = h;
    clamp_pos();
}

void mouse_init(void)
{
    pkt_i = 0;
    synced = 0;
    ev_h = ev_t = 0;
    state.x = bound_w / 2;
    state.y = bound_h / 2;
    state.dx = state.dy = 0;
    state.buttons = 0;
    state.buttons_prev = 0;
    state.ready = 0;

    /* enable auxiliary device */
    ps2_write_cmd(0xA8);

    /* controller config: enable mouse clock */
    ps2_write_cmd(0x20);
    uint8_t cfg = ps2_read_data();
    cfg |= 0x02;  /* IRQ12 — unused while masked, but enable channel */
    cfg &= ~0x20; /* clear disable mouse clock */
    ps2_write_cmd(0x60);
    ps2_write_data(cfg);

    /* defaults + enable streaming */
    if (ps2_write_mouse(0xF6) != 0xFA)
        return;
    if (ps2_write_mouse(0xF4) != 0xFA)
        return;

    state.ready = 1;
}

void mouse_handle_byte(uint8_t b)
{
    if (!state.ready) {
        /* still accept bytes once enabled mid-stream */
        state.ready = 1;
    }

    if (pkt_i == 0) {
        /* bit 3 must be set in first byte of a valid packet */
        if (!(b & 0x08))
            return;
        synced = 1;
    }
    if (!synced)
        return;

    pkt[pkt_i++] = b;
    if (pkt_i < 3)
        return;
    pkt_i = 0;

    uint8_t flags = pkt[0];
    int32_t dx = (int32_t)pkt[1];
    int32_t dy = (int32_t)pkt[2];

    if (flags & 0x10)
        dx |= ~0xFF; /* sign extend X */
    if (flags & 0x20)
        dy |= ~0xFF; /* sign extend Y */
    if (flags & 0x40 || flags & 0x80) {
        /* overflow — drop */
        return;
    }

    /* PS/2 Y is opposite of screen Y */
    dy = -dy;

    state.dx = dx;
    state.dy = dy;
    state.x += dx;
    state.y += dy;
    clamp_pos();

    state.buttons_prev = state.buttons;
    state.buttons = flags & 0x07;

    mouse_event_t ev;
    ev.x = state.x;
    ev.y = state.y;
    ev.buttons = state.buttons;

    if (dx || dy) {
        ev.type = MOUSE_EV_MOVE;
        ev.button = 0;
        ev_push(&ev);
    }

    uint8_t changed = state.buttons ^ state.buttons_prev;
    for (int i = 0; i < 3; i++) {
        uint8_t bit = (uint8_t)(1u << i);
        if (!(changed & bit))
            continue;
        ev.button = bit;
        ev.type = (state.buttons & bit) ? MOUSE_EV_DOWN : MOUSE_EV_UP;
        ev_push(&ev);
    }
}

const mouse_state_t *mouse_get(void)
{
    return &state;
}

int mouse_pop_event(mouse_event_t *ev)
{
    if (!ev || ev_h == ev_t)
        return 0;
    *ev = evq[ev_t];
    ev_t = (ev_t + 1) % MOUSE_EVQ;
    return 1;
}
