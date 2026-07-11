#include <kernel/scheduler.h>
#include <kernel/process.h>
#include <arch/x86/gdt.h>

static uint32_t *bootstrap_esp;

static process_t *pick_next(process_t *cur)
{
    process_t *table = process_table();

    if (!cur) {
        for (int i = 0; i < PROC_MAX; i++) {
            if (table[i].state == PROC_READY)
                return &table[i];
        }
        return NULL;
    }

    int start = (int)(cur - table);
    if (start < 0 || start >= PROC_MAX)
        start = 0;

    for (int i = 1; i <= PROC_MAX; i++) {
        int idx = (start + i) % PROC_MAX;
        if (table[idx].state == PROC_READY)
            return &table[idx];
    }

    if (cur->state == PROC_READY || cur->state == PROC_RUNNING)
        return cur;

    return NULL;
}

void scheduler_init(void)
{
    bootstrap_esp = NULL;
}

void schedule(void)
{
    process_t *cur = process_current();
    process_t *next = pick_next(cur);

    if (!next) {
        for (;;)
            __asm__ volatile("hlt");
    }

    if (cur && cur->state == PROC_RUNNING)
        cur->state = PROC_READY;

    next->state = PROC_RUNNING;

    if (cur == next) {
        process_set_current(next);
        gdt_set_kernel_stack(next->kstack_top);
        return;
    }

    process_set_current(next);
    gdt_set_kernel_stack(next->kstack_top);

    if (cur)
        context_switch(&cur->esp, next->esp);
    else
        context_switch(&bootstrap_esp, next->esp);
}

void scheduler_on_exit(process_t *p)
{
    (void)p;
}

void scheduler_start(void)
{
    process_set_current(NULL);
    schedule();
    for (;;)
        __asm__ volatile("hlt");
}
