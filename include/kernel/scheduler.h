#ifndef MYKERNEL_SCHEDULER_H
#define MYKERNEL_SCHEDULER_H

#include <kernel/types.h>
#include <kernel/process.h>

struct cpu;

void scheduler_init(void);
void scheduler_start(void);   /* never returns — run ready queue forever */
void schedule(void);          /* cooperative yield to next ready process */
void scheduler_unlock_new_thread(void); /* first entry after context_switch */
void scheduler_wake_sleepers(uint64_t now);
void scheduler_on_exit(process_t *p);
void scheduler_on_timer(void);
int  scheduler_current_is_idle(void);
int  scheduler_has_runnable_apps(void);
int  scheduler_create_idle_for_cpu(struct cpu *c);
uint64_t scheduler_tick_count(void);
uint64_t scheduler_idle_ticks(void);

#endif
