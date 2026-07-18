#include <kernel/scheduler.h>
#include <kernel/process.h>
#include <kernel/service.h>
#include <kernel/spinlock.h>
#include <kernel/smp.h>
#include <kernel/string.h>
#include <kernel/sync.h>
#include <kernel/mkdx_api.h>
#include <arch/x86/gdt.h>
#include <arch/x86/irq.h>
#include <arch/x86/cpu.h>
#include <drivers/driver.h>

/*
 * Max timeslice before forced preemption (PIT ticks @ 100Hz).
 * Short slices thrash when several Ready threads exist; 4 ≈ 40ms is enough
 * for fairness without CS storms. Blocked threads stay off Ready.
 */
#define SCHED_TIMESLICE_TICKS 4

static uint32_t *bootstrap_esp[CPU_MAX];
static uint64_t g_switch_ticks;
static spinlock_t g_sched_lock;
/* 0 until scheduler_start — timer must not idle_halt/abandon kernel_main. */
static volatile int g_sched_active;
static uint32_t g_slice_left[CPU_MAX];

static int proc_runnable(process_t *p, uint64_t now)
{
    if (!p || p->state == PROC_UNUSED || p->state == PROC_ZOMBIE)
        return 0;
    if (p->state == PROC_BLOCKED) {
        /* Event-only waits use wake_tick == ~0ULL — never timer-runnable. */
        if (p->wake_tick == ~(uint64_t)0)
            return 0;
        return p->wake_tick <= now;
    }
    return p->state == PROC_READY || p->state == PROC_RUNNING;
}

static int proc_can_run_on(process_t *p, int cpu, process_t *cur)
{
    if (!p)
        return 0;
    /* Idle threads are per-CPU. */
    if (p->is_idle && p->cpu_affinity >= 0 && p->cpu_affinity != cpu)
        return 0;
    /* Another CPU still owns this task. */
    if (p->state == PROC_RUNNING && p != cur)
        return 0;
    if (p->cpu_affinity >= 0 && p->cpu_affinity != cpu && !p->is_idle)
        return 0;
    return 1;
}

/* Prefer threads last run on this CPU (soft affinity), then any Ready. */
static process_t *pick_next(process_t *cur, int cpu)
{
    process_t **table = process_table();
    uint64_t now = irq_timer_ticks();
    process_t *fallback_idle = NULL;
    process_t *soft = NULL;
    process_t *any = NULL;
    int start;

    start = cur && cur->slot >= 0 && cur->slot < PROC_MAX ? cur->slot : 0;

    for (int i = 0; i < PROC_MAX; i++) {
        int idx = (start + i) % PROC_MAX;
        process_t *p = table[idx];
        if (!proc_runnable(p, now) || !proc_can_run_on(p, cpu, cur))
            continue;
        if (p->is_idle) {
            if (!fallback_idle)
                fallback_idle = p;
            continue;
        }
        if (p->cpu == cpu && !soft)
            soft = p;
        if (!any)
            any = p;
        if (soft && any && soft != any)
            break;
    }

    if (soft)
        return soft;
    if (any)
        return any;
    if (cur && proc_runnable(cur, now) && proc_can_run_on(cur, cpu, cur) &&
        !cur->is_idle)
        return cur;
    return fallback_idle;
}

static void idle_halt(void)
{
    for (;;) {
        __asm__ volatile("sti; hlt" ::: "memory");
        /*
         * BSP drains PS/2 then refreshes the software cursor / wakes input
         * waiters — apps may be PROC_BLOCKED in wait_events (C21/I08/I11).
         */
        if (cpu_id() == 0) {
            const mkdx_api_t *api;

            drivers_poll();
            api = mkdx_api_get();
            if (api && api->pump_input)
                api->pump_input();
        }
        if (scheduler_has_runnable_apps())
            break;
    }
}

static void idle_thread(void)
{
    for (;;) {
        if (scheduler_has_runnable_apps()) {
            schedule();
            continue;
        }
        idle_halt();
        schedule();
    }
}

int scheduler_create_idle_for_cpu(cpu_t *c)
{
    pid_t pid;
    process_t *idle;
    char name[12];

    if (!c)
        return -1;

    name[0] = 'i';
    name[1] = 'd';
    name[2] = 'l';
    name[3] = 'e';
    name[4] = '-';
    name[5] = (char)('0' + (c->id % 10));
    name[6] = '\0';

    pid = process_create(name, idle_thread);
    if (pid <= 0)
        return -1;
    idle = process_get(pid);
    if (!idle)
        return -1;
    idle->is_idle = 1;
    idle->cpu_affinity = c->id;
    c->idle = idle;
    return 0;
}

void scheduler_init(void)
{
    cpu_t *bsp = cpu_get(0);

    spin_init(&g_sched_lock);
    memset(bootstrap_esp, 0, sizeof(bootstrap_esp));
    memset(g_slice_left, 0, sizeof(g_slice_left));
    g_switch_ticks = 0;
    g_sched_active = 0;

    if (!bsp)
        bsp = cpu_current();
    (void)scheduler_create_idle_for_cpu(bsp);
}

void scheduler_unlock_new_thread(void)
{
    /* Fresh stacks never return into schedule()'s unlock path. */
    spin_unlock(&g_sched_lock);
    __asm__ volatile("sti" ::: "memory");
}

void scheduler_wake_sleepers(uint64_t now)
{
    process_t **table = process_table();
    uint32_t flags = spin_lock_irqsave(&g_sched_lock);

    for (int i = 0; i < PROC_MAX; i++) {
        process_t *p = table[i];
        if (!p || p->state != PROC_BLOCKED)
            continue;
        if (p->wake_tick == ~(uint64_t)0)
            continue;
        if (p->wake_tick <= now)
            process_wake(p);
    }

    spin_unlock_irqrestore(&g_sched_lock, flags);
}

void schedule(void)
{
    process_t *cur;
    process_t *next;
    int cpu;
    uint32_t **old_esp;
    uint32_t flags;

    /*
     * Before scheduler_start, irq_init has STI + PIT. A premature schedule()
     * from the timer would idle_halt forever (no apps yet) and never return
     * to kernel_main — boot hangs right after heap logs.
     */
    if (!g_sched_active)
        return;

    cpu = cpu_id();

    process_reap_graveyard();
    service_reap_dead();

    cur = process_current();
    if (cur && cur->kill_pending && !cur->is_idle) {
        if (cur->group)
            process_thread_exit(cur->exit_code ? cur->exit_code : 137);
        else
            process_exit(137);
        return;
    }

    /*
     * Hold the lock across context_switch so another CPU cannot pick `cur`
     * while its stack is still live (unlock-before-switch → #UD).
     * irqsave avoids timer nesting that would deadlock on the same lock.
     * The thread switched-to unlocks (resume below, or new-thread trampoline).
     */
    flags = spin_lock_irqsave(&g_sched_lock);
    cur = process_current();

    next = pick_next(cur, cpu);

    /* Halt for timer/IPI when every app is blocked. Without sti+hlt the CPU
     * spins with IF=0 and wake_sleepers never runs. */
    if (!next || (next->is_idle && !scheduler_has_runnable_apps())) {
        spin_unlock_irqrestore(&g_sched_lock, flags);
        idle_halt();
        flags = spin_lock_irqsave(&g_sched_lock);
        cur = process_current();
        next = pick_next(cur, cpu);
    }

    if (!next) {
        spin_unlock_irqrestore(&g_sched_lock, flags);
        return;
    }

    if (cur && cur->state == PROC_RUNNING)
        cur->state = PROC_READY;

    next->state = PROC_RUNNING;
    next->cpu = cpu;

    if (cur == next) {
        process_set_current(next);
        gdt_set_kernel_stack(next->kstack_top);
        spin_unlock_irqrestore(&g_sched_lock, flags);
        return;
    }

    /* Real context switch only — not every timer peek at the same thread. */
    if (cur && !cur->is_idle)
        g_switch_ticks++;

    process_set_current(next);
    gdt_set_kernel_stack(next->kstack_top);

    if (cur)
        old_esp = &cur->esp;
    else
        old_esp = &bootstrap_esp[cpu];

    if (scheduler_has_runnable_apps())
        smp_reschedule_others();

    context_switch(old_esp, next->esp);
    /* Resumed here when some other CPU switches back to this thread. */
    spin_unlock_irqrestore(&g_sched_lock, flags);
}

void scheduler_on_exit(process_t *p)
{
    (void)p;
}

void scheduler_on_timer(void)
{
    process_t **table;
    process_t *cur;
    uint64_t now;
    int need_ipi = 0;
    int cpu;
    int runnable_apps = 0;

    /* PIT runs during bring-up; never preempt until scheduler_start. */
    if (!g_sched_active)
        return;

    /* One CPU tick of work per timer IRQ — not once per schedule() peek. */
    cur = process_current();
    if (cur && !cur->is_idle && cur->state == PROC_RUNNING)
        process_account_tick(cur);

    /* Wake APs only when some non-idle task might run there. */
    table = process_table();
    now = irq_timer_ticks();
    cpu = cpu_id();

    for (int i = 0; i < PROC_MAX; i++) {
        process_t *p = table[i];
        if (!p || p->is_idle)
            continue;
        if (!proc_runnable(p, now))
            continue;
        runnable_apps++;
        /* Wake other CPUs when a task can run off this core. */
        if (p->cpu_affinity < 0 || p->cpu_affinity != cpu)
            need_ipi = 1;
    }
    if (need_ipi)
        smp_reschedule_others();

    /*
     * No Ready apps → stay on idle / HLT; do not burn CS flipping idles.
     * Forced preemption only when another app could run.
     * Input wakes must not wait out the timeslice while the BSP is HLT-idling
     * on a blocked wait_events stack (otherwise dock/menubar feel frozen).
     */
    if (runnable_apps == 0)
        return;

    if (input_event_need_sched() || scheduler_current_is_idle() ||
        (cur && cur->state != PROC_RUNNING)) {
        if (cpu >= 0 && cpu < CPU_MAX)
            g_slice_left[cpu] = 0;
    }

    if (cpu >= 0 && cpu < CPU_MAX && g_slice_left[cpu] > 0)
        g_slice_left[cpu]--;
    if (cpu >= 0 && cpu < CPU_MAX && g_slice_left[cpu] > 0)
        return;
    if (cpu >= 0 && cpu < CPU_MAX)
        g_slice_left[cpu] = SCHED_TIMESLICE_TICKS;

    schedule();
}

int scheduler_current_is_idle(void)
{
    process_t *cur = process_current();
    return cur && cur->is_idle;
}

int scheduler_has_runnable_apps(void)
{
    process_t **table = process_table();
    uint64_t now = irq_timer_ticks();
    int cpu = cpu_id();

    for (int i = 0; i < PROC_MAX; i++) {
        process_t *p = table[i];
        if (!p || p->is_idle)
            continue;
        if (proc_runnable(p, now) && proc_can_run_on(p, cpu, process_current()))
            return 1;
    }
    return 0;
}

uint64_t scheduler_tick_count(void)
{
    return irq_timer_ticks();
}

uint64_t scheduler_idle_ticks(void)
{
    return irq_idle_ticks();
}

void scheduler_start(void)
{
    /* Enable preemption before releasing APs / entering the run loop. */
    __asm__ volatile("" ::: "memory");
    g_sched_active = 1;
    smp_start_scheduling();
    process_set_current(NULL);
    schedule();
    for (;;)
        idle_halt();
}
