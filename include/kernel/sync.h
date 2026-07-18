#ifndef MYKERNEL_SYNC_H
#define MYKERNEL_SYNC_H

#include <kernel/types.h>
#include <kernel/process.h>

/* Kernel auto-reset events — waiters leave the Ready queue until signal/timeout. */
#define KEVENT_MAX 128

/* timeout_ticks: <0 = forever; 0 = try (non-blocking); >0 = max wait. */
#define KEVENT_WAIT_FOREVER ((long)-1)

void sync_init(void);
void sync_cleanup_process(pid_t pid);

/* Returns event id (>=0) or -errno. */
int  kevent_create(void);
int  kevent_destroy(int id);
/* 0 = signaled; -ETIMEDOUT; -errno. */
long kevent_wait(int id, long timeout_ticks);
/* Wake one waiter (or sticky-signal if none). */
long kevent_signal(int id);
/* Wake every waiter. */
long kevent_broadcast(int id);

/* Block current thread until wake_tick (or forever if wake_tick == ~0ULL). */
void process_block(uint64_t wake_tick);
/* Move BLOCKED → READY; safe from IRQ / other CPU contexts. */
void process_wake(process_t *p);

/* Input / WM event wait — apps block until real work (not timed sleep). */
#define INPUT_EV_MOVE   (1u << 0)
#define INPUT_EV_BUTTON (1u << 1)
#define INPUT_EV_WHEEL  (1u << 2)
#define INPUT_EV_KEY    (1u << 3)
#define INPUT_EV_FOCUS  (1u << 4)
#define INPUT_EV_WM     (1u << 5)

uint32_t input_event_seq(void);
/* Called from MKDX when pointer/key/focus/wm state changes. */
void     input_event_notify(uint32_t flags, int hit_id, int focus_id,
                            int prev_hit_id, int wm_id);
/* 1 once after input waiters were woken — timer should force schedule. */
int      input_event_need_sched(void);
/*
 * Block until seq advances with an event relevant to win_id (-1 = any),
 * or timeout. timeout_ticks: <0 forever, 0 try. Returns current seq.
 */
long     input_event_wait(int win_id, uint32_t last_seq, long timeout_ticks);

#endif
