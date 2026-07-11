#include <kernel/syscall.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/vfs.h>

typedef struct {
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags;
} regs_t;

void syscall_isr_handler(regs_t *r)
{
    r->eax = (uint32_t)syscall_dispatch(
        (long)r->eax,
        (long)r->ebx,
        (long)r->ecx,
        (long)r->edx,
        (long)r->esi,
        (long)r->edi);
}

static long do_exit(long code)
{
    process_exit((int)code);
    return 0;
}

static long do_read(long fd, long buf, long count)
{
    process_t *p = process_current();
    if (!p)
        return -1;
    int vfd = process_lookup_fd(p, (int)fd);
    if (vfd < 0)
        return -1;
    return (long)vfs_read(vfd, (void *)buf, (size_t)count);
}

static long do_write(long fd, long buf, long count)
{
    process_t *p = process_current();
    if (!p)
        return -1;
    int vfd = process_lookup_fd(p, (int)fd);
    if (vfd < 0)
        return -1;
    return (long)vfs_write(vfd, (const void *)buf, (size_t)count);
}

static long do_open(long path, long flags)
{
    process_t *p = process_current();
    if (!p)
        return -1;

    int vfd = vfs_open((const char *)path, (int)flags);
    if (vfd < 0)
        return -1;

    int ufd = process_alloc_fd(p, vfd);
    if (ufd < 0) {
        vfs_close(vfd);
        return -1;
    }
    return ufd;
}

static long do_close(long fd)
{
    process_t *p = process_current();
    if (!p)
        return -1;

    int vfd = process_lookup_fd(p, (int)fd);
    if (vfd < 0)
        return -1;

    if ((int)fd > STDERR_FILENO)
        vfs_close(vfd);

    process_free_fd(p, (int)fd);
    return 0;
}

static long do_getpid(void)
{
    process_t *p = process_current();
    return p ? (long)p->pid : -1;
}

static long do_yield(void)
{
    schedule();
    return 0;
}

long syscall_dispatch(long n, long a1, long a2, long a3, long a4, long a5)
{
    (void)a4;
    (void)a5;

    switch (n) {
    case SYS_EXIT:   return do_exit(a1);
    case SYS_READ:   return do_read(a1, a2, a3);
    case SYS_WRITE:  return do_write(a1, a2, a3);
    case SYS_OPEN:   return do_open(a1, a2);
    case SYS_CLOSE:  return do_close(a1);
    case SYS_GETPID: return do_getpid();
    case SYS_YIELD:  return do_yield();
    case SYS_FORK:   return -1;
    default:         return -1;
    }
}

void syscall_init(void)
{
}

static long syscall0(long n)
{
    long ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n) : "memory");
    return ret;
}

static long syscall1(long n, long a1)
{
    long ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "b"(a1) : "memory");
    return ret;
}

static long syscall3(long n, long a1, long a2, long a3)
{
    long ret;
    __asm__ volatile("int $0x80"
                 : "=a"(ret)
                 : "a"(n), "b"(a1), "c"(a2), "d"(a3)
                 : "memory");
    return ret;
}

long sys_exit(int code) { return syscall1(SYS_EXIT, code); }
long sys_getpid(void)   { return syscall0(SYS_GETPID); }
long sys_yield(void)    { return syscall0(SYS_YIELD); }

long sys_read(int fd, void *buf, size_t count)
{
    return syscall3(SYS_READ, fd, (long)buf, (long)count);
}

long sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, fd, (long)buf, (long)count);
}

long sys_open(const char *path, int flags)
{
    long ret;
    __asm__ volatile("int $0x80"
                 : "=a"(ret)
                 : "a"(SYS_OPEN), "b"(path), "c"(flags)
                 : "memory");
    return ret;
}

long sys_close(int fd) { return syscall1(SYS_CLOSE, fd); }
