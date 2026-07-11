#ifndef MYKERNEL_PROCESS_H
#define MYKERNEL_PROCESS_H

#include <kernel/types.h>
#include <kernel/vfs.h>

#define PROC_MAX        8
#define PROC_STACK_SIZE 4096
#define PROC_NAME_MAX   32

typedef enum {
    PROC_UNUSED = 0,
    PROC_READY,
    PROC_RUNNING,
    PROC_BLOCKED,
    PROC_ZOMBIE
} proc_state_t;

typedef struct process {
    pid_t        pid;
    proc_state_t state;
    char         name[PROC_NAME_MAX];
    uint32_t    *stack_base;
    uint32_t    *esp;          /* saved stack pointer for context switch */
    int          exit_code;
    int          fds[VFS_MAX_FD]; /* -1 = empty, else vfs fd index */
} process_t;

void       process_init(void);
process_t *process_current(void);
void       process_set_current(process_t *p);
process_t *process_table(void);
process_t *process_get(pid_t pid);
pid_t      process_create(const char *name, void (*entry)(void));
void       process_exit(int code);
int        process_alloc_fd(process_t *p, int vfs_fd);
int        process_lookup_fd(process_t *p, int user_fd);
void       process_free_fd(process_t *p, int user_fd);

/* asm: switch stacks */
void context_switch(uint32_t **old_esp, uint32_t *new_esp);

#endif
