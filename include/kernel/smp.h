#ifndef MYKERNEL_SMP_H
#define MYKERNEL_SMP_H

#include <kernel/types.h>

/* Probe LAPIC, start APs, create per-CPU idle threads. Call after scheduler_init. */
void smp_init(void);

/* Let APs leave the boot barrier and enter the scheduler. */
void smp_start_scheduling(void);

/* Wake other CPUs from HLT so they can schedule runnable work. */
void smp_reschedule_others(void);

int smp_cpu_count(void);

#endif
