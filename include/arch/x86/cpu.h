#ifndef MYKERNEL_ARCH_CPU_H
#define MYKERNEL_ARCH_CPU_H

#include <kernel/types.h>

#define CPU_MAX 8

struct process;

typedef struct cpu {
    int            id;       /* dense index 0..n-1 */
    uint8_t        apic_id;
    volatile int   online;
    struct process *current;
    struct process *idle;
    uint32_t      *boot_stack;
    int            started;  /* AP entered C runtime */
} cpu_t;

void   cpu_init_bsp(void);
cpu_t *cpu_current(void);
int    cpu_id(void);
int    cpu_count(void);
cpu_t *cpu_get(int id);
cpu_t *cpu_by_apic(uint8_t apic_id);
cpu_t *cpu_alloc(uint8_t apic_id);
void   cpu_rollback_last(void);

#endif
