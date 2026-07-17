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
#define SYS_LSEEK    19
#define SYS_GETPID   20
#define SYS_MKDIR    39
#define SYS_MOUNT    21
#define SYS_UMOUNT   22
#define SYS_YIELD    158   /* sched_yield */

/* Private async I/O */
#define SYS_AIO_SUBMIT 220
#define SYS_AIO_WAIT   221

/* Graphics / window syscalls (mykernel private range) */
#define SYS_GX_INFO          200
#define SYS_GX_PRESENT       201
#define SYS_WM_CREATE        202
#define SYS_WM_DESTROY       203
#define SYS_WM_MAP           204
#define SYS_WM_MOVE          205
#define SYS_WM_RESIZE        206
#define SYS_WM_FOCUS         207
#define SYS_WM_SHOW          208
#define SYS_GX_FILL          209
#define SYS_GX_FILL_ROUND    210
#define SYS_GX_SET_WALLPAPER  211
#define SYS_INPUT_STATE      212
#define SYS_WM_POP_KEY       213
#define SYS_GX_DAMAGE        214
#define SYS_WM_GET_FRAME     215
#define SYS_WM_FIND          216

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
