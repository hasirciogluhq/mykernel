#ifndef MYKERNEL_KERNEL_MODULE_H
#define MYKERNEL_KERNEL_MODULE_H

#include <kernel/types.h>
#include <multiboot.h>

#define KMOD_NAME_MAX 32

typedef int (*kmod_init_fn)(void);

typedef struct kmod {
    char          name[KMOD_NAME_MAX];
    void         *base;
    size_t        size;
    kmod_init_fn  init;
    int           loaded;
} kmod_t;

int  modules_load_blob(const char *name, const void *data, size_t size);
int  modules_load_from_mbi(multiboot_info_t *mbi);
int  modules_load_initrd(const void *data, size_t size);
int  module_load_path(const char *path); /* via vfs.kmod when available */
kmod_t *module_find(const char *name);

#endif
