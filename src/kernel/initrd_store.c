#include <kernel/initrd_store.h>

static const void *g_initrd;
static size_t g_initrd_size;

void initrd_store_set(const void *data, size_t size)
{
    g_initrd = data;
    g_initrd_size = size;
}

const void *initrd_store_get(size_t *size_out)
{
    if (size_out)
        *size_out = g_initrd_size;
    return g_initrd;
}
