#include <kernel/block_api.h>

static const block_api_t *g_block;

void block_api_register(const block_api_t *api)
{
    g_block = api;
}

const block_api_t *block_api_get(void)
{
    return g_block;
}
