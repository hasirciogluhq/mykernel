#pragma once

#include <kernel/types.h>
#include <kernel/vfs.h>
#include <kernel/syscall.h>
#include <user/sdk/syscall.hpp>

#ifndef S_IFMT
#define S_IFMT  0170000
#define S_IFREG 0100000
#define S_IFDIR 0040000
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif

namespace hsrc::sdk {

inline long open(const char *path, int flags)
{
    return syscall2(SYS_OPEN, (long)path, flags);
}

inline long close(int fd)
{
    return syscall1(SYS_CLOSE, fd);
}

inline long read(int fd, void *buf, size_t count)
{
    return syscall3(SYS_READ, fd, (long)buf, (long)count);
}

inline long write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, fd, (long)buf, (long)count);
}

inline long lseek(int fd, long off, int whence)
{
    return syscall3(SYS_LSEEK, fd, off, whence);
}

inline long mkdir(const char *path, int mode = 0755)
{
    return syscall2(SYS_MKDIR, (long)path, mode);
}

inline long chdir(const char *path)
{
    return syscall1(SYS_CHDIR, (long)path);
}

inline long getcwd(char *buf, size_t size)
{
    return syscall2(SYS_GETCWD, (long)buf, (long)size);
}

/* Returns number of dirent entries copied into buf[0..count). */
inline long getdents(int fd, vfs_dirent_t *buf, size_t count)
{
    return syscall3(SYS_GETDENTS, fd, (long)buf, (long)count);
}

inline long getuid()
{
    return syscall0(SYS_GETUID);
}

inline long geteuid()
{
    return syscall0(SYS_GETEUID);
}

inline long getpid()
{
    return syscall0(SYS_GETPID);
}

} // namespace hsrc::sdk
