#ifndef MYKERNEL_SYSCALL_H
#define MYKERNEL_SYSCALL_H

#include <kernel/types.h>

/* Linux i386 syscall numbers (subset) */
#define SYS_EXIT     1
#define SYS_FORK     2
#define SYS_READ     3
#define SYS_WRITE    4
#define SYS_OPEN     5
#define SYS_CLOSE    6
#define SYS_GETPID   20
#define SYS_YIELD    158   /* sched_yield */

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

void syscall_init(void);

/* Called from ISR with register frame in eax.. */
long syscall_dispatch(long n, long a1, long a2, long a3, long a4, long a5);

/* libc-style wrappers for kernel tasks */
long sys_exit(int code);
long sys_read(int fd, void *buf, size_t count);
long sys_write(int fd, const void *buf, size_t count);
long sys_open(const char *path, int flags);
long sys_close(int fd);
long sys_getpid(void);
long sys_yield(void);

#endif
