#ifndef MYKERNEL_TYPES_H
#define MYKERNEL_TYPES_H

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;

typedef signed char        int8_t;
typedef short              int16_t;
typedef int                int32_t;
typedef long long          int64_t;

typedef uint32_t size_t;
typedef int32_t  ssize_t;
typedef int32_t  pid_t;
typedef int32_t  off_t;
typedef uint32_t uid_t;
typedef uint32_t gid_t;
typedef uint32_t uintptr_t;

#define NULL ((void *)0)

#ifndef __cplusplus
#define true  1
#define false 0
typedef int bool;
#endif

#endif
