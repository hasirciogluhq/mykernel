#include <drivers/vfs_fs.h>
#include <kernel/heap.h>
#include <kernel/string.h>
#include <kernel/types.h>

#define DCACHE_SIZE 256

static dentry_t *g_dhash[DCACHE_SIZE];

uint32_t vfs_d_hash(const char *name, size_t len)
{
    uint32_t h = 5381;
    size_t i;
    for (i = 0; i < len; i++)
        h = ((h << 5) + h) + (uint8_t)name[i];
    return h;
}

void vfs_dcache_init(void)
{
    memset(g_dhash, 0, sizeof(g_dhash));
}

dentry_t *vfs_dcache_lookup(dentry_t *parent, const char *name)
{
    uint32_t h;
    dentry_t *d;
    size_t len;

    if (!parent || !name)
        return NULL;
    len = strlen(name);
    h = vfs_d_hash(name, len) & (DCACHE_SIZE - 1);
    for (d = g_dhash[h]; d; d = d->d_hash_next) {
        if (d->d_parent == parent && d->d_cached &&
            strcmp(d->d_name, name) == 0) {
            if (d->d_op && d->d_op->d_revalidate &&
                d->d_op->d_revalidate(d) == 0)
                continue;
            d->d_ref++;
            return d;
        }
    }
    return NULL;
}

void vfs_dcache_add(dentry_t *d)
{
    uint32_t h;
    if (!d || !d->d_name[0])
        return;
    h = vfs_d_hash(d->d_name, strlen(d->d_name)) & (DCACHE_SIZE - 1);
    d->d_hash = h;
    d->d_cached = 1;
    d->d_hash_next = g_dhash[h];
    g_dhash[h] = d;
}

void vfs_dcache_remove(dentry_t *d)
{
    dentry_t **pp;
    if (!d || !d->d_cached)
        return;
    pp = &g_dhash[d->d_hash & (DCACHE_SIZE - 1)];
    while (*pp) {
        if (*pp == d) {
            *pp = d->d_hash_next;
            d->d_cached = 0;
            d->d_hash_next = NULL;
            return;
        }
        pp = &(*pp)->d_hash_next;
    }
}
