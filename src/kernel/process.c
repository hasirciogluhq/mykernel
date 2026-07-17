#include <kernel/process.h>
#include <kernel/string.h>
#include <kernel/scheduler.h>
#include <kernel/syscall.h>
#include <kernel/vfs.h>
#include <arch/x86/gdt.h>

static process_t processes[PROC_MAX];
static uint32_t  kstacks[PROC_MAX][PROC_KSTACK_SIZE / sizeof(uint32_t)];
static uint32_t  ustacks[PROC_MAX][PROC_USTACK_SIZE / sizeof(uint32_t)];
static pid_t     next_pid = 1;
static process_t *current;

static void process_trampoline(void (*entry)(void))
{
    entry();
    process_exit(0);
}

static void user_trampoline(void (*entry)(void))
{
    process_t *p = process_current();
    gdt_set_kernel_stack(p->kstack_top);
    enter_usermode((uint32_t)entry, p->ustack_top);
}

static process_t *alloc_process(const char *name)
{
    process_t *p = NULL;
    int idx = -1;

    for (int i = 0; i < PROC_MAX; i++) {
        if (processes[i].state == PROC_UNUSED) {
            p = &processes[i];
            idx = i;
            break;
        }
    }
    if (!p)
        return NULL;

    memset(p, 0, sizeof(*p));
    p->pid = next_pid++;
    p->state = PROC_READY;
    strncpy(p->name, name, PROC_NAME_MAX - 1);
    p->kstack_base = kstacks[idx];
    p->kstack_top = (uint32_t)(kstacks[idx] + (PROC_KSTACK_SIZE / sizeof(uint32_t)));
    p->ustack_top = (uint32_t)(ustacks[idx] + (PROC_USTACK_SIZE / sizeof(uint32_t)));
    for (int f = 0; f < VFS_MAX_FD; f++)
        p->fds[f] = -1;

    /* Default credentials: root. Shell / desktop spawn with full rights. */
    p->uid = 0;
    p->euid = 0;
    strcpy(p->cwd, "/");

    int cfd = vfs_open("/dev/console", O_RDWR);
    if (cfd >= 0) {
        p->fds[STDIN_FILENO] = cfd;
        p->fds[STDOUT_FILENO] = cfd;
        p->fds[STDERR_FILENO] = cfd;
    }

    return p;
}

static void setup_kstack(process_t *p, void (*trampoline)(void (*)(void)), void (*entry)(void))
{
    uint32_t *sp = (uint32_t *)p->kstack_top;
    *--sp = (uint32_t)entry;
    *--sp = 0;
    *--sp = (uint32_t)trampoline;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    p->esp = sp;
}

void process_init(void)
{
    memset(processes, 0, sizeof(processes));
    for (int i = 0; i < PROC_MAX; i++) {
        processes[i].state = PROC_UNUSED;
        for (int f = 0; f < VFS_MAX_FD; f++)
            processes[i].fds[f] = -1;
    }
    current = NULL;
}

process_t *process_current(void) { return current; }
void process_set_current(process_t *p) { current = p; }
process_t *process_table(void) { return processes; }

process_t *process_get(pid_t pid)
{
    for (int i = 0; i < PROC_MAX; i++) {
        if (processes[i].state != PROC_UNUSED && processes[i].pid == pid)
            return &processes[i];
    }
    return NULL;
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
    setup_kstack(p, user_trampoline, entry);
    return p->pid;
}

void process_exit(int code)
{
    if (!current)
        return;
    current->exit_code = code;
    current->state = PROC_ZOMBIE;
    scheduler_on_exit(current);
    schedule();
    for (;;)
        __asm__ volatile("hlt");
}

int process_kill(pid_t pid)
{
    process_t *p = process_get(pid);
    if (!p || p->state == PROC_UNUSED || p->state == PROC_ZOMBIE)
        return -1;
    if (!p->is_user)
        return -1; /* kernel threads only exit themselves */
    if (p == current)
        process_exit(137);
    p->state = PROC_ZOMBIE;
    p->exit_code = 137;
    return 0;
}

int process_alloc_fd(process_t *p, int vfs_fd)
{
    for (int i = 0; i < VFS_MAX_FD; i++) {
        if (p->fds[i] < 0) {
            p->fds[i] = vfs_fd;
            return i;
        }
    }
    return -1;
}

int process_lookup_fd(process_t *p, int user_fd)
{
    if (!p || user_fd < 0 || user_fd >= VFS_MAX_FD)
        return -1;
    return p->fds[user_fd];
}

void process_free_fd(process_t *p, int user_fd)
{
    if (!p || user_fd < 0 || user_fd >= VFS_MAX_FD)
        return;
    p->fds[user_fd] = -1;
}
