#include <kernel/vfs_api.h>

static const vfs_api_t *g_vfs;

void vfs_api_register(const vfs_api_t *api)
{
    g_vfs = api;
}

const vfs_api_t *vfs_api_get(void)
{
    return g_vfs;
}
