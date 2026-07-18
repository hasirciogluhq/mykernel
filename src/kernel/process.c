#include <kernel/process.h>
#include <kernel/env.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/bootmem.h>
#include <kernel/string.h>
#include <kernel/scheduler.h>
#include <kernel/sync.h>
#include <kernel/socket.h>
#include <kernel/syscall.h>
#include <kernel/vfs.h>
#include <kernel/mkdx_api.h>
#include <kernel/smp.h>
#include <drivers/serial.h>
#include <arch/x86/gdt.h>
#include <arch/x86/cpu.h>
#include <arch/x86/lapic.h>

/* Slot table is pointers only — process_t + stacks come from heap / freelist. */
static process_t *g_procs[PROC_MAX];
static process_t *g_proc_freelist;
/* Exited (ppid==0) procs wait here until schedule() runs on another stack. */
static process_t *g_exit_graveyard;
static pid_t      next_pid = 1;

/* Identity-mapped snapshot for userspace (see SYS_PROC_MAP). */
static proc_page_t g_proc_page;
static int         g_proc_page_dirty = 1;
static uint64_t    g_proc_page_last_tick;

static void process_push_freelist(process_t *p)
{
    if (!p)
        return;
    p->state = PROC_UNUSED;
    p->slot = -1;
    p->free_next = g_proc_freelist;
    g_proc_freelist = p;
}

static process_t *process_pop_freelist(void)
{
    process_t *p = g_proc_freelist;
    if (!p)
        return NULL;
    g_proc_freelist = p->free_next;
    p->free_next = NULL;
    return p;
}

static process_t *process_alloc_struct(void)
{
    process_t *p = process_pop_freelist();
    uint32_t *kbase;
    uint32_t *ubase;

    if (p)
        return p;

    p = (process_t *)kmalloc(sizeof(*p));
    if (!p)
        return NULL;
    memset(p, 0, sizeof(*p));

    kbase = (uint32_t *)kmalloc_aligned(PROC_KSTACK_SIZE, 16);
    ubase = (uint32_t *)kmalloc_aligned(PROC_USTACK_SIZE, 16);
    if (!kbase || !ubase) {
        /* Bump heap cannot reclaim partial failure; refuse the slot. */
        return NULL;
    }
    p->kstack_base = kbase;
    p->ustack_base = ubase;
    return p;
}

static void process_clear_slot(process_t *p)
{
    uint32_t *kbase;
    uint32_t *ubase;
    int slot;

    if (!p)
        return;

    kbase = p->kstack_base;
    ubase = p->ustack_base;
    slot = p->slot;

    if (slot >= 0 && slot < PROC_MAX && g_procs[slot] == p)
        g_procs[slot] = NULL;

    memset(p, 0, sizeof(*p));
    p->kstack_base = kbase;
    p->ustack_base = ubase;
    for (int fd = 0; fd < VFS_MAX_FD; fd++)
        p->fds[fd] = -1;

    process_push_freelist(p);
    process_snapshot_mark_dirty();
}

static uint32_t process_stack_bytes(const process_t *p)
{
    if (!p || p->state == PROC_UNUSED)
        return 0;
    uint32_t total = PROC_KSTACK_SIZE;
    if (p->is_user)
        total += PROC_USTACK_SIZE;
    return total;
}

static uint32_t process_vma_bytes(const process_t *p)
{
    uint32_t total = 0;

    if (!p || p->state == PROC_UNUSED)
        return 0;

    for (int i = 0; i < VMA_MAX; i++) {
        if (!p->vmas[i].used)
            continue;
        total += (uint32_t)(p->vmas[i].npages * PAGE_SIZE);
    }
    return total;
}

static uint32_t process_mem_bytes(const process_t *p)
{
    uint32_t total = 0;

    if (!p || p->state == PROC_UNUSED)
        return 0;

    total += (uint32_t)sizeof(process_t);
    total += process_stack_bytes(p);
    total += p->image_bytes;
    total += process_vma_bytes(p);

    return total;
}

static uint64_t process_sum_cpu_ticks(void)
{
    uint64_t sum = 0;

    for (int i = 0; i < PROC_MAX; i++) {
        process_t *p = g_procs[i];
        if (!p || p->state == PROC_UNUSED)
            continue;
        sum += p->cpu_ticks;
    }
    return sum;
}

static uint64_t process_uptime_ticks(const process_t *p, uint64_t now_ticks)
{
    if (!p || now_ticks < p->start_ticks)
        return 0;
    return now_ticks - p->start_ticks;
}

static void process_free_console(pid_t pid)
{
    const mkdx_api_t *api = mkdx_api_get();
    if (api && api->console_free)
        api->console_free((int)pid);
}

static void process_free_windows(pid_t pid)
{
    const mkdx_api_t *api = mkdx_api_get();
    if (api && api->wm_destroy_by_pid)
        api->wm_destroy_by_pid((int)pid);
}

static void process_release_fds(process_t *p)
{
    if (!p)
        return;

    for (int i = 0; i < VFS_MAX_FD; i++) {
        int fd = p->fds[i];
        if (fd < 0)
            continue;
        if (PROC_FD_IS_SOCK(fd))
            (void)sock_close(PROC_FD_SOCK_ID(fd));
        else
            (void)vfs_close(fd);
        p->fds[i] = -1;
    }
}

static void process_trampoline(void (*entry)(void))
{
    scheduler_unlock_new_thread();
    entry();
    process_exit(0);
}

static void user_trampoline(void (*entry)(void))
{
    process_t *p;

    scheduler_unlock_new_thread();
    p = process_current();
    /* Authoritative entry — stack arg can be stale after context_switch. */
    if (p && p->user_entry)
        entry = p->user_entry;
    klog("[user] enter name=");
    klog(p && p->name[0] ? p->name : "?");
    klog(" entry=");
    serial_print_hex((uint32_t)(uintptr_t)entry);
    klog(" ustack=");
    serial_print_hex(p ? p->ustack_top : 0);
    klog("\n");
    if (!p || !entry) {
        klog("[user] FATAL: bad trampoline state\n");
        for (;;)
            __asm__ volatile("hlt");
    }
    gdt_set_kernel_stack(p->kstack_top);
    enter_usermode((uint32_t)entry, p->ustack_top);
    klog("[user] FATAL: enter_usermode returned\n");
    for (;;)
        __asm__ volatile("hlt");
}

/* Extra threads: cdecl entry(void *arg) with arg on the user stack. */
static void user_thread_trampoline(void (*entry)(void))
{
    process_t *t;
    uint32_t *usp;

    scheduler_unlock_new_thread();
    t = process_current();
    if (t && t->user_entry)
        entry = t->user_entry;
    if (!t || !entry) {
        klog("[user] FATAL: bad thread trampoline\n");
        for (;;)
            __asm__ volatile("hlt");
    }
    gdt_set_kernel_stack(t->kstack_top);
    usp = (uint32_t *)t->ustack_top;
    *--usp = (uint32_t)(uintptr_t)t->thread_arg;
    *--usp = 0; /* ret — SDK bootstrap calls SYS_THREAD_EXIT */
    enter_usermode((uint32_t)(uintptr_t)entry, (uint32_t)(uintptr_t)usp);
    for (;;)
        __asm__ volatile("hlt");
}

process_t *process_leader(process_t *p)
{
    if (!p)
        return NULL;
    return p->group ? p->group : p;
}

static int process_count_threads(process_t *lead)
{
    int n = 0;

    if (!lead)
        return 0;
    for (int i = 0; i < PROC_MAX; i++) {
        process_t *p = g_procs[i];
        if (!p || p->state == PROC_UNUSED)
            continue;
        if (p == lead || p->group == lead)
            n++;
    }
    return n;
}

static void process_wake_joiners(pid_t tid)
{
    for (int i = 0; i < PROC_MAX; i++) {
        process_t *p = g_procs[i];
        if (!p || p->state != PROC_BLOCKED || p->join_tid == 0)
            continue;
        if (p->join_tid == tid)
            process_wake(p);
    }
}

static void process_kill_thread_slot(process_t *t, int code)
{
    if (!t || t->state == PROC_UNUSED || t->state == PROC_ZOMBIE)
        return;
    if (t->state == PROC_RUNNING && t->cpu >= 0 && t->cpu != cpu_id()) {
        cpu_t *remote = cpu_get(t->cpu);
        t->kill_pending = 1;
        t->exit_code = code;
        if (remote)
            lapic_send_ipi(remote->apic_id, LAPIC_IPI_VECTOR);
        return;
    }
    t->exit_code = code;
    t->kill_pending = 0;
    t->state = PROC_ZOMBIE;
    t->cpu = -1;
    process_wake_joiners(t->tid);
    if (t->thread_detached)
        process_clear_slot(t);
}

/* Parents (os-ui) never waitpid — reap zombies so PROC_MAX does not fill. */
static void process_reap_zombies(void)
{
    for (int i = 0; i < PROC_MAX; i++) {
        process_t *p = g_procs[i];
        if (!p || p->state != PROC_ZOMBIE)
            continue;
        process_release_fds(p);
        process_clear_slot(p);
    }
}

static int process_find_slot(void)
{
    for (int i = 0; i < PROC_MAX; i++) {
        if (!g_procs[i])
            return i;
    }
    return -1;
}

static process_t *alloc_process(const char *name)
{
    process_t *p;
    uint32_t *kbase;
    uint32_t *ubase;
    int idx;

    idx = process_find_slot();
    if (idx < 0) {
        process_reap_zombies();
        idx = process_find_slot();
    }
    if (idx < 0)
        return NULL;

    p = process_alloc_struct();
    if (!p)
        return NULL;

    kbase = p->kstack_base;
    ubase = p->ustack_base;
    memset(p, 0, sizeof(*p));
    p->kstack_base = kbase;
    p->ustack_base = ubase;
    p->slot = idx;
    p->pid = next_pid++;
    p->tid = p->pid; /* main thread: tid == pid */
    {
        process_t *cur = process_current();
        process_t *lead = process_leader(cur);
        p->ppid = lead ? lead->pid : 0;
        p->state = PROC_READY;
        p->cpu = -1;
        p->cpu_affinity = -1;
        p->kill_pending = 0;
        p->group = NULL;
        p->thread_arg = NULL;
        p->thread_detached = 0;
        p->join_tid = 0;
        p->wait_event = -1;
        p->wait_next = NULL;
        strncpy(p->name, name, PROC_NAME_MAX - 1);
        p->kstack_top = (uint32_t)((uint8_t *)kbase + PROC_KSTACK_SIZE);
        p->ustack_top = (uint32_t)((uint8_t *)ubase + PROC_USTACK_SIZE);
        p->start_ticks = scheduler_tick_count();
        for (int f = 0; f < VFS_MAX_FD; f++)
            p->fds[f] = -1;

        if (lead) {
            p->uid = lead->uid;
            p->euid = lead->euid;
            strncpy(p->cwd, lead->cwd, sizeof(p->cwd) - 1);
        } else {
            /* Default credentials: root. Shell / desktop spawn with full rights. */
            p->uid = 0;
            p->euid = 0;
            strcpy(p->cwd, "/");
        }

        env_inherit(p, lead);
    }

    int cfd = vfs_open("/dev/console", O_RDWR);
    if (cfd >= 0) {
        p->fds[STDIN_FILENO] = cfd;
        p->fds[STDOUT_FILENO] = cfd;
        p->fds[STDERR_FILENO] = cfd;
    }

    g_procs[idx] = p;
    process_snapshot_mark_dirty();
    return p;
}

static void setup_kstack(process_t *p, void (*trampoline)(void (*)(void)), void (*entry)(void))
{
    uint32_t *sp = (uint32_t *)p->kstack_top;
    /*
     * context_switch pops ebx..ebp then ret → trampoline.
     * cdecl trampoline(void (*entry)(void)) expects:
     *   [esp]     = return address (unused dummy)
     *   [esp+4]   = entry
     * Low → high: ebx, esi, edi, ebp, trampoline, dummy_ret, entry
     */
    *--sp = (uint32_t)entry;
    *--sp = 0; /* dummy return address for cdecl */
    *--sp = (uint32_t)trampoline;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    p->esp = sp;
}

void process_init(void)
{
    memset(g_procs, 0, sizeof(g_procs));
    g_proc_freelist = NULL;
    g_exit_graveyard = NULL;
    memset(&g_proc_page, 0, sizeof(g_proc_page));
    g_proc_page.magic = PROC_PAGE_MAGIC;
    g_proc_page.seq = 0;
    g_proc_page_dirty = 1;
    g_proc_page_last_tick = 0;
}

void process_reap_graveyard(void)
{
    process_t *cur = process_current();
    process_t **pp = &g_exit_graveyard;

    while (*pp) {
        process_t *p = *pp;
        if (p == cur) {
            pp = &p->free_next;
            continue;
        }
        *pp = p->free_next;
        p->free_next = NULL;
        process_push_freelist(p);
    }
}

proc_page_t *process_page_get(void)
{
    return &g_proc_page;
}

void process_snapshot_mark_dirty(void)
{
    g_proc_page_dirty = 1;
}

static void process_wake_proc_waiters(uint32_t gen)
{
    for (int i = 0; i < PROC_MAX; i++) {
        process_t *p = g_procs[i];
        if (!p || p->state != PROC_BLOCKED || p->proc_wait_gen == 0)
            continue;
        if (gen != p->proc_wait_gen)
            process_wake(p);
    }
}

void process_snapshot_publish(void)
{
    uint64_t now = scheduler_tick_count();
    uint32_t count = 0;
    uint32_t filled = 0;
    uint32_t used_ram;
    uint32_t free_ram;
    int was_dirty = g_proc_page_dirty;

    /*
     * Only republish when the table changed, or the previous sample is stale.
     * Callers (SYS_PROC_MAP / PROC_LIST) invoke this on demand — not per-yield.
     */
    if (!was_dirty && g_proc_page_last_tick != 0 &&
        now - g_proc_page_last_tick < 16)
        return;

    free_ram = (uint32_t)heap_free();
    used_ram = bootmem_total_ram();
    if (used_ram < free_ram)
        used_ram = (uint32_t)heap_used();
    else
        used_ram -= free_ram;

    g_proc_page.seq++; /* odd = write in progress */
    __asm__ volatile("" ::: "memory");

    for (int i = 0; i < PROC_MAX; i++) {
        process_t *p = g_procs[i];
        if (!p || p->state == PROC_UNUSED)
            continue;
        /* Skip unreaped zombies — they clutter Activity Monitor as "Zombie". */
        if (p->state == PROC_ZOMBIE)
            continue;
        if (p->is_idle || p->group)
            continue;
        if (filled < PROC_PAGE_MAX) {
            proc_page_entry_t *dst = &g_proc_page.entries[filled++];
            uint64_t ticks = p->cpu_ticks;
            uint32_t stacks = process_stack_bytes(p);

            for (int j = 0; j < PROC_MAX; j++) {
                process_t *th = g_procs[j];
                if (!th || th->group != p || th->state == PROC_UNUSED)
                    continue;
                ticks += th->cpu_ticks;
                stacks += process_stack_bytes(th);
            }

            memset(dst, 0, sizeof(*dst));
            dst->pid = p->pid;
            dst->ppid = p->ppid;
            dst->state = (uint32_t)p->state;
            dst->is_user = (uint32_t)p->is_user;
            dst->cpu_ticks = ticks;
            dst->uptime_ticks = process_uptime_ticks(p, now);
            dst->stack_bytes = stacks;
            dst->image_bytes = p->image_bytes;
            dst->vma_bytes = process_vma_bytes(p);
            dst->mem_bytes = process_mem_bytes(p) + (stacks - process_stack_bytes(p));
            strncpy(dst->name, p->name, sizeof(dst->name) - 1);
        }
        count++;
    }

    g_proc_page.count = filled;
    /* Drop stale rows from the previous sample (count may have shrunk). */
    if (filled < PROC_PAGE_MAX)
        memset(&g_proc_page.entries[filled], 0,
               (size_t)(PROC_PAGE_MAX - filled) * sizeof(g_proc_page.entries[0]));
    g_proc_page.process_count = count;
    g_proc_page.uptime_ticks = now;
    g_proc_page.total_cpu_ticks = process_sum_cpu_ticks();
    g_proc_page.idle_ticks = scheduler_idle_ticks();
    g_proc_page.used_ram_bytes = used_ram;
    g_proc_page.free_ram_bytes = free_ram;
    g_proc_page.total_ram_bytes = bootmem_total_ram();
    if (was_dirty) {
        g_proc_page.generation++;
        process_wake_proc_waiters(g_proc_page.generation);
    }

    __asm__ volatile("" ::: "memory");
    g_proc_page.seq++; /* even = stable */
    g_proc_page_dirty = 0;
    g_proc_page_last_tick = now;
}

process_t *process_current(void)
{
    return cpu_current()->current;
}

void process_set_current(process_t *p)
{
    cpu_t *c = cpu_current();
    c->current = p;
    if (p)
        p->cpu = c->id;
}

process_t **process_table(void) { return g_procs; }

process_t *process_get(pid_t pid)
{
    for (int i = 0; i < PROC_MAX; i++) {
        process_t *p = g_procs[i];
        if (p && p->state != PROC_UNUSED && p->pid == pid && !p->group)
            return p;
    }
    return NULL;
}

process_t *process_by_tid(pid_t tid)
{
    for (int i = 0; i < PROC_MAX; i++) {
        process_t *p = g_procs[i];
        if (p && p->state != PROC_UNUSED && p->tid == tid)
            return p;
    }
    return NULL;
}

static process_t *thread_get(pid_t tid)
{
    return process_by_tid(tid);
}

pid_t process_create(const char *name, void (*entry)(void))
{
    process_t *p = alloc_process(name);
    if (!p)
        return -1;
    p->is_user = 0;
    p->user_entry = NULL;
    setup_kstack(p, process_trampoline, entry);
    return p->pid;
}

pid_t process_create_user(const char *name, void (*entry)(void))
{
    process_t *p = alloc_process(name);
    if (!p)
        return -1;
    p->is_user = 1;
    p->user_entry = entry;
    /*
     * Pin user/GUI processes to BSP. MKDX compose/present, PS/2 poll, and
     * display_ops are not SMP-safe: running a GX syscall on an AP while the
     * BSP timer idle path calls drivers_poll()/pump_input() races and
     * corrupts kernel state (seen as #UD with a garbage EIP like 0x207
     * right after the first full gx present).
     */
    p->cpu_affinity = 0;
    setup_kstack(p, user_trampoline, entry);
    return p->pid;
}

pid_t process_getpid(void)
{
    process_t *lead = process_leader(process_current());
    return lead ? lead->pid : 0;
}

pid_t process_getppid(void)
{
    process_t *lead = process_leader(process_current());
    return lead ? lead->ppid : 0;
}

pid_t process_gettid(void)
{
    process_t *cur = process_current();
    return cur ? cur->tid : 0;
}

pid_t process_thread_create(void (*entry)(void *), void *arg)
{
    process_t *cur = process_current();
    process_t *lead;
    process_t *t;
    int cfd;

    if (!cur || !entry)
        return (pid_t)-EINVAL;
    lead = process_leader(cur);
    if (!lead || !lead->is_user)
        return (pid_t)-EPERM;
    if (process_count_threads(lead) >= PROC_THREADS_MAX)
        return (pid_t)-EAGAIN;

    t = alloc_process(lead->name);
    if (!t)
        return (pid_t)-ENOMEM;

    /* Drop console fds opened by alloc_process — share leader resources. */
    process_release_fds(t);

    t->group = lead;
    t->tid = t->pid;
    t->ppid = lead->ppid;
    t->is_user = 1;
    t->user_entry = (void (*)(void))(uintptr_t)entry;
    t->thread_arg = arg;
    t->thread_detached = 0;
    t->join_tid = 0;
    t->cpu_affinity = lead->cpu_affinity;
    t->uid = lead->uid;
    t->euid = lead->euid;
    t->image_bytes = 0;
    strncpy(t->cwd, lead->cwd, sizeof(t->cwd) - 1);
    memset(&t->env, 0, sizeof(t->env));
    memset(&t->argv, 0, sizeof(t->argv));
    memset(t->vmas, 0, sizeof(t->vmas));

    /* Keep stdin/out/err usable via leader lookup; no private console. */
    (void)cfd;

    setup_kstack(t, user_thread_trampoline, (void (*)(void))(uintptr_t)entry);
    process_snapshot_mark_dirty();
    smp_reschedule_others();
    return t->tid;
}

void process_thread_exit(int code)
{
    process_t *cur = process_current();

    if (!cur)
        return;

    /* Main thread exit tears down the whole process. */
    if (!cur->group) {
        process_exit(code);
        return;
    }

    cur->exit_code = code;
    cur->kill_pending = 0;
    cur->state = PROC_ZOMBIE;
    cur->cpu = -1;
    process_wake_joiners(cur->tid);
    if (cur->thread_detached) {
        int slot = cur->slot;
        if (slot >= 0 && slot < PROC_MAX && g_procs[slot] == cur)
            g_procs[slot] = NULL;
        cur->state = PROC_UNUSED;
        cur->slot = -1;
        cur->free_next = g_exit_graveyard;
        g_exit_graveyard = cur;
    }
    process_snapshot_mark_dirty();
    schedule();
    for (;;)
        __asm__ volatile("hlt");
}

long process_thread_join(pid_t tid, int *status_out)
{
    process_t *cur = process_current();
    process_t *lead;
    process_t *t;

    if (!cur || tid <= 0)
        return -EINVAL;
    lead = process_leader(cur);
    t = thread_get(tid);
    if (!t || process_leader(t) != lead)
        return -ESRCH;
    if (t == cur)
        return -EINVAL;
    if (t->thread_detached)
        return -EINVAL;

    while (t->state != PROC_ZOMBIE && t->state != PROC_UNUSED) {
        cur->join_tid = tid;
        process_block(~(uint64_t)0);
        schedule();
        cur->join_tid = 0;
        if (cur->kill_pending)
            return -EINTR;
        t = thread_get(tid);
        if (!t)
            return -ESRCH;
    }

    if (status_out)
        *status_out = t->exit_code;
    if (t->state == PROC_ZOMBIE)
        process_clear_slot(t);
    return 0;
}

long process_thread_detach(pid_t tid)
{
    process_t *cur = process_current();
    process_t *lead;
    process_t *t;

    if (!cur || tid <= 0)
        return -EINVAL;
    lead = process_leader(cur);
    t = thread_get(tid);
    if (!t || process_leader(t) != lead)
        return -ESRCH;
    t->thread_detached = 1;
    if (t->state == PROC_ZOMBIE)
        process_clear_slot(t);
    return 0;
}

pid_t process_waitpid(pid_t pid, int *status_out, int options)
{
    int found_child = 0;
    process_t *cur = process_leader(process_current());
    (void)options;

    if (!cur)
        return -ESRCH;

    for (int i = 0; i < PROC_MAX; i++) {
        process_t *p = g_procs[i];
        pid_t child_pid;

        if (!p || p->state == PROC_UNUSED || p->group || p->ppid != cur->pid)
            continue;
        if (pid > 0 && p->pid != pid)
            continue;

        found_child = 1;
        if (p->state != PROC_ZOMBIE)
            continue;

        child_pid = p->pid;
        if (status_out)
            *status_out = p->exit_code;
        process_release_fds(p);
        process_clear_slot(p);
        return child_pid;
    }

    if (!found_child)
        return -ECHILD;
    return 0;
}

void process_exit(int code)
{
    pid_t pid;
    process_t *cur = process_current();
    process_t *lead;

    if (!cur)
        return;

    /* Secondary thread: SYS_EXIT still ends the whole process. */
    lead = process_leader(cur);
    if (cur->group && lead && lead != cur) {
        /* Ask leader to exit; if we are not on leader stack, kill group here. */
        for (int i = 0; i < PROC_MAX; i++) {
            process_t *t = g_procs[i];
            if (!t || t == cur || t->state == PROC_UNUSED)
                continue;
            if (t == lead || t->group == lead)
                process_kill_thread_slot(t, code);
        }
    } else {
        for (int i = 0; i < PROC_MAX; i++) {
            process_t *t = g_procs[i];
            if (!t || t == cur || t->state == PROC_UNUSED)
                continue;
            if (t->group == cur)
                process_kill_thread_slot(t, code);
        }
        lead = cur;
    }

    pid = lead->pid;
    sync_cleanup_process(pid);
    process_release_fds(lead);
    process_free_windows(pid);
    process_free_console(pid);
    lead->exit_code = code;
    lead->kill_pending = 0;
    cur->exit_code = code;
    cur->kill_pending = 0;

    if (cur != lead) {
        /* Exiting from a worker: park this stack in the graveyard. */
        int slot = cur->slot;
        if (slot >= 0 && slot < PROC_MAX && g_procs[slot] == cur)
            g_procs[slot] = NULL;
        cur->state = PROC_UNUSED;
        cur->slot = -1;
        cur->cpu = -1;
        cur->free_next = g_exit_graveyard;
        g_exit_graveyard = cur;
    }

    if (lead->ppid == 0) {
        /*
         * Detach from the table but keep struct/stack alive until
         * process_reap_graveyard() runs on another process's stack.
         */
        int slot = lead->slot;
        if (slot >= 0 && slot < PROC_MAX && g_procs[slot] == lead)
            g_procs[slot] = NULL;
        lead->state = PROC_UNUSED;
        lead->slot = -1;
        lead->cpu = -1;
        if (lead != cur) {
            lead->free_next = g_exit_graveyard;
            g_exit_graveyard = lead;
        } else {
            lead->free_next = g_exit_graveyard;
            g_exit_graveyard = lead;
        }
    } else {
        lead->state = PROC_ZOMBIE;
        lead->cpu = -1;
    }
    process_snapshot_mark_dirty();
    process_snapshot_publish();
    scheduler_on_exit(lead);
    schedule();
    for (;;)
        __asm__ volatile("hlt");
}

int process_kill(pid_t pid)
{
    process_t *p = process_get(pid);
    process_t *cur = process_current();

    if (!p || p->state == PROC_UNUSED || p->state == PROC_ZOMBIE)
        return -ESRCH;
    if (!p->is_user)
        return -EPERM; /* kernel threads only exit themselves */
    if (process_leader(cur) == p)
        process_exit(137);

    for (int i = 0; i < PROC_MAX; i++) {
        process_t *t = g_procs[i];
        if (!t || t == p || t->state == PROC_UNUSED)
            continue;
        if (t->group == p)
            process_kill_thread_slot(t, 137);
    }

    /* Running on another CPU — ask it to exit at next schedule point. */
    if (p->state == PROC_RUNNING && p->cpu >= 0 && p->cpu != cpu_id()) {
        cpu_t *remote = cpu_get(p->cpu);
        p->kill_pending = 1;
        if (remote)
            lapic_send_ipi(remote->apic_id, LAPIC_IPI_VECTOR);
        smp_reschedule_others();
        return 0;
    }
    sync_cleanup_process(p->pid);
    process_release_fds(p);
    process_free_windows(p->pid);
    process_free_console(p->pid);
    p->exit_code = 137;
    if (p->ppid == 0) {
        /*
         * Boot-spawned orphans (ppid==0): detach like process_exit so we do
         * not memset a struct whose stack may still be frozen mid-run.
         */
        int slot = p->slot;
        if (slot >= 0 && slot < PROC_MAX && g_procs[slot] == p)
            g_procs[slot] = NULL;
        p->state = PROC_UNUSED;
        p->slot = -1;
        p->free_next = g_exit_graveyard;
        g_exit_graveyard = p;
    } else {
        p->state = PROC_ZOMBIE;
    }
    process_snapshot_mark_dirty();
    process_snapshot_publish();
    return 0;
}

void process_account_tick(process_t *p)
{
    if (!p || p->state == PROC_UNUSED || p->is_idle)
        return;
    p->cpu_ticks++;
}

int process_list_range(proc_list_entry_t *out, size_t max_entries, size_t skip)
{
    uint64_t now_ticks = scheduler_tick_count();
    size_t total = 0;
    size_t filled = 0;

    for (int i = 0; i < PROC_MAX; i++) {
        process_t *p = g_procs[i];

        if (!p || p->state == PROC_UNUSED || p->group)
            continue;
        if (p->is_idle)
            continue;

        if (total >= skip && out && filled < max_entries) {
            proc_list_entry_t *dst = &out[filled++];
            uint64_t ticks = p->cpu_ticks;
            uint32_t stacks = process_stack_bytes(p);

            for (int j = 0; j < PROC_MAX; j++) {
                process_t *th = g_procs[j];
                if (!th || th->group != p || th->state == PROC_UNUSED)
                    continue;
                ticks += th->cpu_ticks;
                stacks += process_stack_bytes(th);
            }

            memset(dst, 0, sizeof(*dst));
            dst->pid = p->pid;
            dst->ppid = p->ppid;
            dst->state = (uint32_t)p->state;
            dst->is_user = (uint32_t)p->is_user;
            dst->cpu_ticks = ticks;
            dst->uptime_ticks = process_uptime_ticks(p, now_ticks);
            dst->stack_bytes = stacks;
            dst->image_bytes = p->image_bytes;
            dst->vma_bytes = process_vma_bytes(p);
            dst->mem_bytes = process_mem_bytes(p) + (stacks - process_stack_bytes(p));
            strncpy(dst->name, p->name, sizeof(dst->name) - 1);
        }
        total++;
    }

    return (int)total;
}

int process_list(proc_list_entry_t *out, size_t max_entries)
{
    return process_list_range(out, max_entries, 0);
}

int process_stat(pid_t pid, proc_stat_t *out)
{
    process_t *p;
    uint64_t now_ticks = scheduler_tick_count();
    uint64_t ticks;
    uint32_t stacks;

    if (!out)
        return -EFAULT;

    p = pid > 0 ? process_get(pid) : process_leader(process_current());
    if (!p || p->state == PROC_UNUSED)
        return -ESRCH;
    p = process_leader(p);
    if (!p)
        return -ESRCH;

    ticks = p->cpu_ticks;
    stacks = process_stack_bytes(p);
    for (int j = 0; j < PROC_MAX; j++) {
        process_t *th = g_procs[j];
        if (!th || th->group != p || th->state == PROC_UNUSED)
            continue;
        ticks += th->cpu_ticks;
        stacks += process_stack_bytes(th);
    }

    memset(out, 0, sizeof(*out));
    out->pid = p->pid;
    out->ppid = p->ppid;
    out->state = (uint32_t)p->state;
    out->is_user = (uint32_t)p->is_user;
    out->cpu_ticks = ticks;
    out->start_ticks = p->start_ticks;
    out->uptime_ticks = process_uptime_ticks(p, now_ticks);
    out->stack_bytes = stacks;
    out->image_bytes = p->image_bytes;
    out->vma_bytes = process_vma_bytes(p);
    out->mem_bytes = process_mem_bytes(p) + (stacks - process_stack_bytes(p));
    strncpy(out->name, p->name, sizeof(out->name) - 1);
    return 0;
}

int process_sysinfo(sys_info_t *out)
{
    if (!out)
        return -EFAULT;

    memset(out, 0, sizeof(*out));
    out->uptime_ticks = scheduler_tick_count();
    out->total_cpu_ticks = process_sum_cpu_ticks();
    out->idle_ticks = scheduler_idle_ticks();
    out->free_ram_bytes = (uint32_t)heap_free();
    out->total_ram_bytes = bootmem_total_ram();
    if (out->total_ram_bytes < out->free_ram_bytes)
        out->used_ram_bytes = (uint32_t)heap_used();
    else
        out->used_ram_bytes = out->total_ram_bytes - out->free_ram_bytes;

    for (int i = 0; i < PROC_MAX; i++) {
        process_t *p = g_procs[i];
        if (p && p->state != PROC_UNUSED && !p->is_idle && !p->group)
            out->process_count++;
    }

    return 0;
}

int process_alloc_fd(process_t *p, int vfs_fd)
{
    p = process_leader(p);
    if (!p)
        return -1;
    for (int i = 0; i < VFS_MAX_FD; i++) {
        if (p->fds[i] < 0) {
            p->fds[i] = vfs_fd;
            return i;
        }
    }
    return -1;
}

int process_alloc_sock_fd(process_t *p, int sock_id)
{
    p = process_leader(p);
    if (!p)
        return -1;
    for (int i = 0; i < VFS_MAX_FD; i++) {
        if (p->fds[i] < 0) {
            p->fds[i] = PROC_FD_MAKE_SOCK(sock_id);
            return i;
        }
    }
    return -1;
}

int process_lookup_fd(process_t *p, int user_fd)
{
    p = process_leader(p);
    if (!p || user_fd < 0 || user_fd >= VFS_MAX_FD)
        return -1;
    return p->fds[user_fd];
}

void process_free_fd(process_t *p, int user_fd)
{
    p = process_leader(p);
    if (!p || user_fd < 0 || user_fd >= VFS_MAX_FD)
        return;
    p->fds[user_fd] = -1;
}
