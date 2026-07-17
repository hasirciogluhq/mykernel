#ifndef MYKERNEL_ERRNO_H
#define MYKERNEL_ERRNO_H

/* Negative returns: return -ENOENT; */
#define EPERM        1
#define ENOENT       2
#define ESRCH        3
#define EINTR        4
#define EIO          5
#define ENXIO        6
#define E2BIG        7
#define ENOEXEC      8
#define EBADF        9
#define ECHILD      10
#define EAGAIN      11
#define ENOMEM      12
#define EACCES      13
#define EFAULT      14
#define EBUSY       16
#define EEXIST      17
#define ENODEV      19
#define ENOTDIR     20
#define EISDIR      21
#define EINVAL      22
#define ENFILE      23
#define EMFILE      24
#define ENOSPC      28
#define EROFS       30
#define EMLINK      31
#define EPIPE       32
#define ERANGE      34
#define ENAMETOOLONG 36
#define ENOSYS      38
#define ENOTEMPTY   39
#define ELOOP       40
#define EOVERFLOW   75
#define ENOTSUP     95
#define EOPNOTSUPP  ENOTSUP

#endif
