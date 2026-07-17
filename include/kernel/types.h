#ifndef MYKERNEL_TYPES_H
#define MYKERNEL_TYPES_H

/*
 * Match GCC freestanding <stdint.h> / <stddef.h> type identities so usermode
 * code can mix Dear ImGui (stdint) with kernel headers without conflicts.
 */
#if defined(__GNUC__)

typedef __UINT8_TYPE__       uint8_t;
typedef __UINT16_TYPE__      uint16_t;
typedef __UINT32_TYPE__      uint32_t;
typedef __UINT64_TYPE__      uint64_t;

typedef __INT8_TYPE__        int8_t;
typedef __INT16_TYPE__       int16_t;
typedef __INT32_TYPE__       int32_t;
typedef __INT64_TYPE__       int64_t;
typedef __UINTPTR_TYPE__     uintptr_t;

#ifndef _SIZE_T
#ifndef __size_t_defined
typedef __SIZE_TYPE__ size_t;
#define _SIZE_T
#endif
#endif

#else /* !__GNUC__ */

#ifndef _STDINT_H
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;

typedef signed char        int8_t;
typedef short              int16_t;
typedef int                int32_t;
typedef long long          int64_t;
typedef uint32_t           uintptr_t;
#endif

#ifndef _SIZE_T
#ifndef __size_t_defined
typedef uint32_t size_t;
#define _SIZE_T
#endif
#endif

#endif /* __GNUC__ */

typedef int32_t  ssize_t;
typedef int32_t  pid_t;
typedef int32_t  off_t;
typedef uint32_t uid_t;
typedef uint32_t gid_t;

#ifndef NULL
#ifdef __cplusplus
#define NULL 0
#else
#define NULL ((void *)0)
#endif
#endif

#ifndef __cplusplus
#ifndef true
#define true  1
#define false 0
typedef int bool;
#endif
#endif

#endif
