#ifndef MYKERNEL_KERNEL_INITRD_H
#define MYKERNEL_KERNEL_INITRD_H

#include <kernel/types.h>

#define INITRD_MAGIC 0x44525249u /* 'IRRD' */
#define INITRD_NAME_MAX 32
#define INITRD_MAX_FILES 64

typedef struct initrd_file {
    char     name[INITRD_NAME_MAX];
    uint32_t offset;
    uint32_t size;
} initrd_file_t;

typedef struct initrd_header {
    uint32_t     magic;
    uint32_t     count;
    initrd_file_t files[INITRD_MAX_FILES];
} initrd_header_t;

#endif
