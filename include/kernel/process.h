#ifndef MYKERNEL_PROCESS_H
#define MYKERNEL_PROCESS_H

#include <kernel/types.h>
#include <kernel/vfs.h>
#include <kernel/mm.h>
#include <kernel/env.h>
#include <kernel/argv.h>
#include <kernel/proc_abi.h>

/* Hard ceiling — slots are pointers only; structs/stacks grow on demand. */
#define PROC_MAX         8192
#define PROC_KSTACK_SIZE 8192
#define PROC_USTACK_SIZE 8192
#define PROC_NAME_MAX    PROC_PAGE_NAME

/* fds[]: VFS fd (>=0) or socket id tagged with PROC_FD_SOCK. */
#define PROC_FD_SOCK     0x40000000
#define PROC_FD_IS_SOCK(x) (((x) >= 0) && (((unsigned)(x) & PROC_FD_SOCK) != 0))
#define PROC_FD_SOCK_ID(x) ((int)((unsigned)(x) & 0xFF))
#define PROC_FD_MAKE_SOCK(id) ((int)(PROC_FD_SOCK | ((unsigned)(id) & 0xFF)))

typedef enum {
    PROC_UNUSED = 0,
    PROC_READY,
    PROC_RUNNING,
    PROC_BLOCKED,
    PROC_ZOMBIE
} proc_state_t;

typedef proc_page_entry_t proc_list_entry_t;

typedef struct proc_stat {
    pid_t    pid;
    pid_t    ppid;
    uint32_t state;
    uint32_t is_user;
    uint64_t cpu_ticks;
    uint64_t start_ticks;
    uint64_t uptime_ticks;
    uint32_t mem_bytes;
    char     name[PROC_NAME_MAX];
} proc_stat_t;

typedef struct sys_info {
    uint64_t uptime_ticks;
    uint64_t total_cpu_ticks;
    uint32_t total_ram_bytes;
    uint32_t used_ram_bytes;
    uint32_t free_ram_bytes;
    uint32_t process_count;
} sys_info_t;

typedef struct process {
    pid_t        pid;
    pid_t        ppid;
    proc_state_t state;
    char         name[PROC_NAME_MAX];
    int          is_user;      /* 1 = ring-3 userspace */
    uid_t        uid;          /* real uid — default 0 (root) */
    uid_t        euid;         /* effective uid — default 0 (root) */
    char         cwd[VFS_PATH_MAX];
    int          slot;         /* index in process table, or -1 */
    uint32_t    *kstack_base;
    uint32_t    *ustack_base;
    uint32_t     kstack_top;
    uint32_t     ustack_top;
    uint32_t    *esp;          /* saved kernel stack pointer */
    struct process *free_next; /* freelist link when unused */
    void       (*user_entry)(void);
    int          exit_code;
    uint64_t     cpu_ticks;
    uint64_t     start_ticks;
    int          fds[VFS_MAX_FD];
    vma_t        vmas[VMA_MAX];
    proc_env_t   env;
    proc_argv_t  argv;
} process_t;

void        process_init(void);
process_t  *process_current(void);
void        process_set_current(process_t *p);
process_t **process_table(void);
process_t  *process_get(pid_t pid);
void        process_reap_graveyard(void);

pid_t process_create(const char *name, void (*entry)(void));
pid_t process_create_user(const char *name, void (*entry)(void));
pid_t process_getppid(void);
pid_t process_waitpid(pid_t pid, int *status_out, int options);
void  process_exit(int code);
int   process_kill(pid_t pid);
int   process_list(proc_list_entry_t *out, size_t max_entries);
int   process_list_range(proc_list_entry_t *out, size_t max_entries, size_t skip);
int   process_stat(pid_t pid, proc_stat_t *out);
int   process_sysinfo(sys_info_t *out);
void  process_account_tick(process_t *p);

/* Shared snapshot page (SYS_PROC_MAP) — publish throttled from yield. */
proc_page_t *process_page_get(void);
void         process_snapshot_mark_dirty(void);
void         process_snapshot_publish(void);

int  process_alloc_fd(process_t *p, int vfs_fd);
int  process_alloc_sock_fd(process_t *p, int sock_id);
int  process_lookup_fd(process_t *p, int user_fd);
void process_free_fd(process_t *p, int user_fd);

void context_switch(uint32_t **old_esp, uint32_t *new_esp);
void enter_usermode(uint32_t entry, uint32_t user_stack);

#endif
