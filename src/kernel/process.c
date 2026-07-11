#include <kernel/process.h>
#include <kernel/string.h>
#include <kernel/scheduler.h>
#include <kernel/syscall.h>
#include <kernel/vfs.h>

static process_t processes[PROC_MAX];
static uint32_t  stacks[PROC_MAX][PROC_STACK_SIZE / sizeof(uint32_t)];
static pid_t     next_pid = 1;
static process_t *current;

/* trampoline: entry() returns here → exit */
static void process_trampoline(void (*entry)(void))
{
    entry();
    process_exit(0);
}

void process_init(void)
{
    memset(processes, 0, sizeof(processes));
    for (int i = 0; i < PROC_MAX; i++) {
        processes[i].state = PROC_UNUSED;
        processes[i].pid = 0;
        for (int f = 0; f < VFS_MAX_FD; f++)
            processes[i].fds[f] = -1;
    }
    current = NULL;
}

process_t *process_current(void)
{
    return current;
}

void process_set_current(process_t *p)
{
    current = p;
}

process_t *process_table(void)
{
    return processes;
}

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
        return -1;

    memset(p, 0, sizeof(*p));
    p->pid = next_pid++;
    p->state = PROC_READY;
    strncpy(p->name, name, PROC_NAME_MAX - 1);
    p->stack_base = stacks[idx];
    for (int f = 0; f < VFS_MAX_FD; f++)
        p->fds[f] = -1;

    /* Build initial stack for context_switch restore:
     *   pop ebp, edi, esi, ebx; ret → process_trampoline
     * trampoline arg (entry) sits above return address. */
    uint32_t *sp = p->stack_base + (PROC_STACK_SIZE / sizeof(uint32_t));

    *--sp = (uint32_t)entry;                 /* arg to trampoline */
    *--sp = 0;                               /* fake return addr */
    *--sp = (uint32_t)process_trampoline;    /* ret target */
    *--sp = 0;                               /* ebx */
    *--sp = 0;                               /* esi */
    *--sp = 0;                               /* edi */
    *--sp = 0;                               /* ebp */
    p->esp = sp;

    /* stdin/out/err → /dev/console */
    {
        int cfd = vfs_open("/dev/console", O_RDWR);
        if (cfd >= 0) {
            p->fds[STDIN_FILENO] = cfd;
            p->fds[STDOUT_FILENO] = cfd;
            p->fds[STDERR_FILENO] = cfd;
        }
    }

    return p->pid;
}

void process_exit(int code)
{
    if (!current)
        return;

    current->exit_code = code;
    current->state = PROC_ZOMBIE;
    scheduler_on_exit(current);
    schedule(); /* never returns to this process */
    for (;;)
        __asm__ volatile("hlt");
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
