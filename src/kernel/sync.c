#include <kernel/sync.h>
#include <kernel/errno.h>
#include <kernel/scheduler.h>
#include <kernel/smp.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <arch/x86/irq.h>

typedef struct kevent {
    int         used;
    int         signaled; /* sticky until a wait consumes it (auto-reset) */
    pid_t       owner;
    process_t  *waiters;  /* singly linked via process.wait_next */
} kevent_t;

static kevent_t   g_events[KEVENT_MAX];
static spinlock_t g_sync_lock;

static kevent_t *kevent_get(int id)
{
    if (id < 0 || id >= KEVENT_MAX || !g_events[id].used)
        return NULL;
    return &g_events[id];
}

static void waiter_unlink(kevent_t *ev, process_t *p)
{
    process_t **pp;

    if (!ev || !p)
        return;
    for (pp = &ev->waiters; *pp; pp = &(*pp)->wait_next) {
        if (*pp == p) {
            *pp = p->wait_next;
            p->wait_next = NULL;
            p->wait_event = -1;
            return;
        }
    }
}

static void waiter_enqueue(kevent_t *ev, process_t *p)
{
    process_t **pp;

    if (!ev || !p)
        return;
    for (pp = &ev->waiters; *pp; pp = &(*pp)->wait_next) {
        if (*pp == p)
            return;
    }
    p->wait_next = NULL;
    p->wait_event = (int)(ev - g_events);
    *pp = p;
}

static int wake_one_locked(kevent_t *ev)
{
    process_t *p = ev->waiters;
    if (!p)
        return 0;
    ev->waiters = p->wait_next;
    p->wait_next = NULL;
    p->wait_event = -1;
    process_wake(p);
    return 1;
}

static int wake_all_locked(kevent_t *ev)
{
    int n = 0;
    while (wake_one_locked(ev))
        n++;
    return n;
}

void sync_init(void)
{
    spin_init(&g_sync_lock);
    memset(g_events, 0, sizeof(g_events));
    for (int i = 0; i < KEVENT_MAX; i++)
        g_events[i].owner = -1;
}

void sync_cleanup_process(pid_t pid)
{
    uint32_t flags = spin_lock_irqsave(&g_sync_lock);

    for (int i = 0; i < KEVENT_MAX; i++) {
        kevent_t *ev = &g_events[i];
        if (!ev->used)
            continue;

        {
            process_t **pp = &ev->waiters;
            while (*pp) {
                process_t *w = *pp;
                process_t *lead = process_leader(w);
                if (w->pid == pid || (lead && lead->pid == pid)) {
                    *pp = w->wait_next;
                    w->wait_next = NULL;
                    w->wait_event = -1;
                } else {
                    pp = &(*pp)->wait_next;
                }
            }
        }

        if (ev->owner == pid) {
            wake_all_locked(ev);
            memset(ev, 0, sizeof(*ev));
            ev->owner = -1;
        }
    }

    spin_unlock_irqrestore(&g_sync_lock, flags);
}

void process_block(uint64_t wake_tick)
{
    process_t *p = process_current();
    if (!p)
        return;
    p->wake_tick = wake_tick;
    p->state = PROC_BLOCKED;
    process_snapshot_mark_dirty();
}

void process_wake(process_t *p)
{
    if (!p)
        return;
    if (p->state != PROC_BLOCKED)
        return;
    p->state = PROC_READY;
    p->wake_tick = 0;
    p->proc_wait_gen = 0;
    p->input_wait_active = 0;
    process_snapshot_mark_dirty();
}

static uint32_t g_input_seq = 1;
static volatile int g_input_need_sched;

uint32_t input_event_seq(void)
{
    return g_input_seq;
}

int input_event_need_sched(void)
{
    if (!g_input_need_sched)
        return 0;
    g_input_need_sched = 0;
    return 1;
}

static int input_relevant(int win_id, uint32_t flags, int hit_id, int focus_id,
                          int prev_hit_id, int wm_id)
{
    if (win_id < 0)
        return 1;
    if ((flags & INPUT_EV_WM) && wm_id == win_id)
        return 1;
    /* Pointer over this window, leaving it, or this window has focus. */
    if (hit_id == win_id || prev_hit_id == win_id || focus_id == win_id)
        return 1;
    if ((flags & (INPUT_EV_BUTTON | INPUT_EV_KEY | INPUT_EV_WHEEL)) &&
        (focus_id == win_id || hit_id == win_id))
        return 1;
    return 0;
}

void input_event_notify(uint32_t flags, int hit_id, int focus_id,
                        int prev_hit_id, int wm_id)
{
    process_t **table;
    uint32_t flags_irq;
    int woke = 0;

    if (!flags)
        return;

    g_input_seq++;
    if (g_input_seq == 0)
        g_input_seq = 1;

    table = process_table();
    flags_irq = spin_lock_irqsave(&g_sync_lock);
    for (int i = 0; i < PROC_MAX; i++) {
        process_t *p = table[i];
        if (!p || p->state != PROC_BLOCKED || !p->input_wait_active)
            continue;
        if (g_input_seq == p->input_wait_last)
            continue;
        if (!input_relevant(p->input_wait_win, flags, hit_id, focus_id,
                            prev_hit_id, wm_id))
            continue;
        process_wake(p);
        woke = 1;
    }
    spin_unlock_irqrestore(&g_sync_lock, flags_irq);

    if (woke) {
        g_input_need_sched = 1;
        smp_reschedule_others();
    }
}

long input_event_wait(int win_id, uint32_t last_seq, long timeout_ticks)
{
    process_t *cur = process_current();
    uint32_t flags;
    uint64_t now;
    uint64_t wake_at;
    uint32_t cur_seq;

    if (!cur)
        return -ESRCH;

    cur_seq = g_input_seq;
    if (cur_seq != last_seq)
        return (long)cur_seq;

    if (timeout_ticks == 0)
        return (long)cur_seq;

    now = irq_timer_ticks();
    if (timeout_ticks < 0)
        wake_at = ~(uint64_t)0;
    else {
        if (timeout_ticks > 100000)
            timeout_ticks = 100000;
        wake_at = now + (uint64_t)timeout_ticks;
    }

    flags = spin_lock_irqsave(&g_sync_lock);
    /* Lost-wakeup guard: event may have arrived after the unlocked seq check. */
    cur_seq = g_input_seq;
    if (cur_seq != last_seq) {
        spin_unlock_irqrestore(&g_sync_lock, flags);
        return (long)cur_seq;
    }
    cur->input_wait_active = 1;
    cur->input_wait_last = last_seq;
    cur->input_wait_win = win_id;
    process_block(wake_at);
    spin_unlock_irqrestore(&g_sync_lock, flags);

    schedule();

    flags = spin_lock_irqsave(&g_sync_lock);
    cur->input_wait_active = 0;
    spin_unlock_irqrestore(&g_sync_lock, flags);

    return (long)g_input_seq;
}

int kevent_create(void)
{
    process_t *cur = process_current();
    uint32_t flags;
    int id = -1;

    if (!cur)
        return -ESRCH;

    flags = spin_lock_irqsave(&g_sync_lock);
    for (int i = 0; i < KEVENT_MAX; i++) {
        if (!g_events[i].used) {
            process_t *lead = process_leader(cur);
            memset(&g_events[i], 0, sizeof(g_events[i]));
            g_events[i].used = 1;
            g_events[i].owner = lead ? lead->pid : cur->pid;
            id = i;
            break;
        }
    }
    spin_unlock_irqrestore(&g_sync_lock, flags);
    return id >= 0 ? id : -ENOMEM;
}

int kevent_destroy(int id)
{
    process_t *cur = process_current();
    kevent_t *ev;
    uint32_t flags;

    if (!cur)
        return -ESRCH;

    flags = spin_lock_irqsave(&g_sync_lock);
    ev = kevent_get(id);
    if (!ev) {
        spin_unlock_irqrestore(&g_sync_lock, flags);
        return -EBADF;
    }
    {
        process_t *lead = process_leader(cur);
        pid_t owner = lead ? lead->pid : cur->pid;
        if (ev->owner != owner) {
            spin_unlock_irqrestore(&g_sync_lock, flags);
            return -EPERM;
        }
    }
    wake_all_locked(ev);
    memset(ev, 0, sizeof(*ev));
    ev->owner = -1;
    spin_unlock_irqrestore(&g_sync_lock, flags);
    return 0;
}

long kevent_wait(int id, long timeout_ticks)
{
    process_t *cur = process_current();
    kevent_t *ev;
    uint32_t flags;
    uint64_t now;
    uint64_t wake_at;
    int still_queued;

    if (!cur)
        return -ESRCH;

    flags = spin_lock_irqsave(&g_sync_lock);
    ev = kevent_get(id);
    if (!ev) {
        spin_unlock_irqrestore(&g_sync_lock, flags);
        return -EBADF;
    }

    if (ev->signaled) {
        ev->signaled = 0;
        spin_unlock_irqrestore(&g_sync_lock, flags);
        return 0;
    }

    if (timeout_ticks == 0) {
        spin_unlock_irqrestore(&g_sync_lock, flags);
        return -EAGAIN;
    }

    now = irq_timer_ticks();
    if (timeout_ticks < 0)
        wake_at = ~(uint64_t)0;
    else {
        if (timeout_ticks > 100000)
            timeout_ticks = 100000;
        wake_at = now + (uint64_t)timeout_ticks;
    }

    waiter_enqueue(ev, cur);
    process_block(wake_at);
    spin_unlock_irqrestore(&g_sync_lock, flags);

    schedule();

    /*
     * Resume: signal path unlinks us (wait_event == -1) → success.
     * Timer path leaves us linked → timeout; consume sticky if any.
     */
    flags = spin_lock_irqsave(&g_sync_lock);
    still_queued = (cur->wait_event == id);
    ev = kevent_get(id);
    if (still_queued && ev)
        waiter_unlink(ev, cur);
    else {
        cur->wait_event = -1;
        cur->wait_next = NULL;
    }

    if (!still_queued) {
        spin_unlock_irqrestore(&g_sync_lock, flags);
        return 0;
    }

    if (ev && ev->signaled) {
        ev->signaled = 0;
        spin_unlock_irqrestore(&g_sync_lock, flags);
        return 0;
    }

    spin_unlock_irqrestore(&g_sync_lock, flags);
    return -ETIMEDOUT;
}

long kevent_signal(int id)
{
    kevent_t *ev;
    uint32_t flags;
    int woke;

    flags = spin_lock_irqsave(&g_sync_lock);
    ev = kevent_get(id);
    if (!ev) {
        spin_unlock_irqrestore(&g_sync_lock, flags);
        return -EBADF;
    }
    woke = wake_one_locked(ev);
    if (!woke)
        ev->signaled = 1;
    spin_unlock_irqrestore(&g_sync_lock, flags);

    if (woke)
        smp_reschedule_others();
    return 0;
}

long kevent_broadcast(int id)
{
    kevent_t *ev;
    uint32_t flags;
    int n;

    flags = spin_lock_irqsave(&g_sync_lock);
    ev = kevent_get(id);
    if (!ev) {
        spin_unlock_irqrestore(&g_sync_lock, flags);
        return -EBADF;
    }
    n = wake_all_locked(ev);
    if (n == 0)
        ev->signaled = 1;
    spin_unlock_irqrestore(&g_sync_lock, flags);

    if (n > 0)
        smp_reschedule_others();
    return 0;
}
