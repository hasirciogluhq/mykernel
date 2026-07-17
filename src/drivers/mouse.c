#include <drivers/mouse.h>
#include <drivers/ps2.h>

#define MOUSE_EVQ 64

static mouse_state_t state;
static int32_t bound_w = 1024;
static int32_t bound_h = 768;

static uint8_t pkt[4];
static int     pkt_i;
static int     pkt_bytes = 3; /* 3 = standard PS/2, 4 = IntelliMouse/Explorer */
static int     synced;

static mouse_event_t evq[MOUSE_EVQ];
static unsigned ev_h;
static unsigned ev_t;

static void ev_push(mouse_event_t *e)
{
    /* Coalesce consecutive moves so the queue keeps the latest pointer. */
    if (e->type == MOUSE_EV_MOVE && ev_h != ev_t) {
        unsigned last = (ev_h + MOUSE_EVQ - 1u) % MOUSE_EVQ;
        if (evq[last].type == MOUSE_EV_MOVE) {
            evq[last] = *e;
            return;
        }
    }

    unsigned next = (ev_h + 1u) % MOUSE_EVQ;
    if (next == ev_t) {
        /* Full: drop oldest — never drop the newest packet. */
        ev_t = (ev_t + 1u) % MOUSE_EVQ;
    }
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

/* Mild ballistic acceleration — keep fine motion 1:1; only boost large flicks. */
static int32_t accel_delta(int32_t d)
{
    int32_t a = d < 0 ? -d : d;
    int32_t mul;

    if (a >= 16)
        mul = 2;
    else
        mul = 1;
    return d * mul;
}

static int mouse_try_cmd(uint8_t cmd)
{
    return ps2_write_mouse(cmd) == 0xFA ? 0 : -1;
}

static int mouse_try_set_rate(uint8_t hz)
{
    if (mouse_try_cmd(0xF3) < 0)
        return -1;
    return mouse_try_cmd(hz);
}

static int mouse_try_set_res(uint8_t res)
{
    if (mouse_try_cmd(0xE8) < 0)
        return -1;
    return mouse_try_cmd(res);
}

/* IntelliMouse: F3 200 / F3 100 / F3 80 then F2 → ID 3 (wheel) or 4 (Explorer). */
static int mouse_try_intellimouse(void)
{
    uint8_t id;

    if (mouse_try_set_rate(200) < 0)
        return -1;
    if (mouse_try_set_rate(100) < 0)
        return -1;
    if (mouse_try_set_rate(80) < 0)
        return -1;
    if (mouse_try_cmd(0xF2) < 0)
        return -1;
    id = ps2_read_data();
    if (id == 3) {
        state.has_wheel = 1;
        state.has_side_btns = 0;
        pkt_bytes = 4;
        return 0;
    }
    if (id == 4) {
        state.has_wheel = 1;
        state.has_side_btns = 1;
        pkt_bytes = 4;
        return 0;
    }
    return -1;
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
    pkt_bytes = 3;
    synced = 0;
    ev_h = ev_t = 0;
    state.x = bound_w / 2;
    state.y = bound_h / 2;
    state.dx = state.dy = 0;
    state.wheel = 0;
    state.buttons = 0;
    state.buttons_prev = 0;
    state.has_wheel = 0;
    state.has_side_btns = 0;
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

    /* defaults + streaming */
    if (ps2_write_mouse(0xF6) != 0xFA)
        return;

    /* Prefer 4 counts/mm (res=2) — res=3 felt ultra-fast on QEMU when idle.
     * 100Hz is enough; 200Hz flooded the poll path under full-frame presents. */
    (void)mouse_try_set_res(2);

    /* Arm wheel (+ optional side buttons) before final stream enable. */
    if (mouse_try_intellimouse() < 0) {
        if (mouse_try_set_rate(100) < 0)
            (void)mouse_try_set_rate(80);
        pkt_bytes = 3;
        state.has_wheel = 0;
        state.has_side_btns = 0;
    }

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
    if (pkt_i < pkt_bytes)
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

    dx = accel_delta(dx);
    dy = accel_delta(dy);

    state.dx = dx;
    state.dy = dy;
    state.x += dx;
    state.y += dy;
    clamp_pos();

    state.buttons_prev = state.buttons;
    state.buttons = (uint8_t)(flags & 0x07);

    int32_t z = 0;
    if (pkt_bytes >= 4) {
        uint8_t zb = pkt[3];
        if (state.has_side_btns) {
            /* Explorer: byte3 = [0 0 btn5 btn4 Z3 Z2 Z1 Z0] */
            if (zb & 0x10)
                state.buttons |= MOUSE_BTN_BACK;
            if (zb & 0x20)
                state.buttons |= MOUSE_BTN_FORWARD;
            int8_t zn = (int8_t)(zb & 0x0F);
            if (zn & 0x08)
                zn = (int8_t)(zn | (int8_t)0xF0);
            z = (int32_t)zn;
        } else {
            /* IntelliMouse ID=3: full signed wheel byte. */
            z = (int32_t)(int8_t)zb;
        }
        if (z != 0) {
            state.wheel += z;
            mouse_event_t wev;
            wev.type = MOUSE_EV_WHEEL;
            wev.x = state.x;
            wev.y = state.y;
            wev.wheel = z;
            wev.button = 0;
            wev.buttons = state.buttons;
            ev_push(&wev);
        }
    }

    mouse_event_t ev;
    ev.x = state.x;
    ev.y = state.y;
    ev.wheel = 0;
    ev.buttons = state.buttons;

    if (dx || dy) {
        ev.type = MOUSE_EV_MOVE;
        ev.button = 0;
        ev_push(&ev);
    }

    uint8_t changed = state.buttons ^ state.buttons_prev;
    for (int i = 0; i < 5; i++) {
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

int32_t mouse_consume_wheel(void)
{
    int32_t w = state.wheel;
    state.wheel = 0;
    return w;
}

int mouse_pop_event(mouse_event_t *ev)
{
    if (!ev || ev_h == ev_t)
        return 0;
    *ev = evq[ev_t];
    ev_t = (ev_t + 1) % MOUSE_EVQ;
    return 1;
}
