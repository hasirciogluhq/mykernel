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
#define SYS_CHDIR    12
#define SYS_LSEEK    19
#define SYS_GETPID   20
#define SYS_MOUNT    21
#define SYS_UMOUNT   22
#define SYS_GETUID   24
#define SYS_MKDIR    39
#define SYS_GETEUID  49
#define SYS_GETDENTS 141
#define SYS_GETCWD   183
#define SYS_YIELD    158   /* sched_yield */

/* Private async I/O + VFS extras */
#define SYS_AIO_SUBMIT   220
#define SYS_AIO_WAIT     221
#define SYS_MMAP         222
#define SYS_MUNMAP       223
#define SYS_MSYNC        224
#define SYS_FLOCK        225
#define SYS_GETXATTR     226
#define SYS_SETXATTR     227
#define SYS_LISTXATTR    228
#define SYS_REMOVEXATTR  229
#define SYS_FSNOTIFY_ADD 230
#define SYS_FSNOTIFY_RM  231
#define SYS_FSNOTIFY_READ 232
#define SYS_MODULE_LOAD  233
#define SYS_SOCKET       241
#define SYS_BIND         242
#define SYS_CONNECT      243
#define SYS_SENDTO       244
#define SYS_RECVFROM     245
#define SYS_UNLINK       10
#define SYS_RMDIR        40
#define SYS_RENAME       38

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
#define SYS_GX_DAMAGE_RECT   278  /* a1=win a2=&ugx_damage_args (window-local) */
#define SYS_WM_GET_FRAME     215
#define SYS_WM_FIND          216
#define SYS_WM_SET           217
#define SYS_WM_GET           218
#define SYS_WM_CLOSE         219
#define SYS_WM_FIND_CLASS    275  /* a1 = class_name ptr → window id or -1 */

/* Networking control syscalls (Wave F2) */
#define SYS_NETIF_GET        246
#define SYS_NETIF_SET        247
#define SYS_DHCP_RENEW       248
#define SYS_LISTEN           249
#define SYS_ACCEPT           250
#define SYS_SEND             251
#define SYS_RECV             252
#define SYS_SHUTDOWN         253
#define SYS_SPAWN           254
#define SYS_WAITPID         255
#define SYS_GETPPID         256
#define SYS_PROC_LIST       257
#define SYS_PROC_STAT       258
#define SYS_SYSINFO         259
#define SYS_KILL            260
#define SYS_PROC_MAP        276  /* → pointer to proc_page_t (identity) */
#define SYS_PROC_WAIT       277  /* block until proc_page generation changes */
#define SYS_SERVICE_LIST    261
#define SYS_SERVICE_START   262
#define SYS_SERVICE_STOP    263
#define SYS_SERVICE_STATUS  264
#define SYS_GETENV          265
#define SYS_SETENV          266
#define SYS_CONSOLE_SHOW    267
#define SYS_GETARGC         268
#define SYS_GETARGV         269

/* Time (shared page + rare calibration/set). */
#define SYS_TIME_MAP        270  /* → pointer to time_page_t (identity) */
#define SYS_TIME_GET        271  /* copy time_snapshot_t to user */
#define SYS_TIME_SET        272  /* set UTC ns (a1=lo, a2=hi) */
#define SYS_TIME_SETTZ      273  /* a1=offset_sec, a2=name ptr */
#define SYS_TIME_SETFLAGS   274  /* a1=flags */

/* SYS_SPAWN flags (Wave O). */
#define SPAWN_CONSOLE_VISIBLE  0x01u
#define SPAWN_CONSOLE_HIDDEN   0x02u

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
