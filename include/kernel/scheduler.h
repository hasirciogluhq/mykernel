#ifndef MYKERNEL_SCHEDULER_H
#define MYKERNEL_SCHEDULER_H

#include <kernel/types.h>
#include <kernel/process.h>

void scheduler_init(void);
void scheduler_start(void);   /* never returns — run ready queue forever */
void schedule(void);          /* cooperative yield to next ready process */
void scheduler_on_exit(process_t *p);

#endif
