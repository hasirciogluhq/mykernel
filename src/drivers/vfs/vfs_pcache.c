#include <drivers/vfs_fs.h>
#include <kernel/heap.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/types.h>

#define PCACHE_MAX   256
#define PAGE_SHIFT   12
#define PAGE_SIZE    4096u

typedef struct page_cache_entry {
    inode_t  *inode;
    uint64_t  index;
    uint8_t  *data;
    int       dirty;
    int       used;
} page_cache_entry_t;

static page_cache_entry_t g_pcache[PCACHE_MAX];

void vfs_pcache_init(void)
{
    memset(g_pcache, 0, sizeof(g_pcache));
}

static page_cache_entry_t *pc_find(inode_t *inode, uint64_t index)
{
    size_t i;
    for (i = 0; i < PCACHE_MAX; i++) {
        if (g_pcache[i].used && g_pcache[i].inode == inode &&
            g_pcache[i].index == index)
            return &g_pcache[i];
    }
    return NULL;
}

static page_cache_entry_t *pc_alloc(inode_t *inode, uint64_t index)
{
    size_t i;
    page_cache_entry_t *e = pc_find(inode, index);
    if (e)
        return e;
    for (i = 0; i < PCACHE_MAX; i++) {
        if (!g_pcache[i].used) {
            e = &g_pcache[i];
            break;
        }
    }
    if (!e) {
        /* Evict first clean page */
        for (i = 0; i < PCACHE_MAX; i++) {
            if (g_pcache[i].used && !g_pcache[i].dirty) {
                e = &g_pcache[i];
                break;
            }
        }
    }
    if (!e)
        e = &g_pcache[0];
    if (e->used && e->data) {
        if (e->dirty && e->inode && e->inode->i_mapping &&
            e->inode->i_mapping->a_ops && e->inode->i_mapping->a_ops->writepage)
            e->inode->i_mapping->a_ops->writepage(e->inode, e->data, e->index);
    }
    if (!e->data)
        e->data = (uint8_t *)kmalloc(PAGE_SIZE);
    if (!e->data)
        return NULL;
    memset(e->data, 0, PAGE_SIZE);
    e->inode = inode;
    e->index = index;
    e->dirty = 0;
    e->used = 1;
    return e;
}

static int pc_fill(inode_t *inode, page_cache_entry_t *e)
{
    if (!inode || !e)
        return -EINVAL;
    if (inode->i_mapping && inode->i_mapping->a_ops &&
        inode->i_mapping->a_ops->readpage)
        return inode->i_mapping->a_ops->readpage(inode, e->data, e->index);
    /* Fallback: no a_ops — leave zeroed */
    return 0;
}

ssize_t vfs_cached_read(inode_t *inode, void *buf, size_t count, off_t pos)
{
    uint8_t *out = (uint8_t *)buf;
    size_t done = 0;

    if (!inode || !buf)
        return -EINVAL;
    if ((uint64_t)pos >= inode->i_size)
        return 0;
    if (count > inode->i_size - (uint64_t)pos)
        count = (size_t)(inode->i_size - (uint64_t)pos);

    while (done < count) {
        uint64_t index = ((uint64_t)pos + done) >> PAGE_SHIFT;
        size_t poff = (size_t)(((uint64_t)pos + done) & (PAGE_SIZE - 1));
        size_t take = PAGE_SIZE - poff;
        page_cache_entry_t *e;

        if (take > count - done)
            take = count - done;
        e = pc_alloc(inode, index);
        if (!e)
            return done ? (ssize_t)done : -ENOMEM;
        if (pc_fill(inode, e) < 0)
            return -EIO;
        memcpy(out + done, e->data + poff, take);
        done += take;
    }
    return (ssize_t)done;
}

ssize_t vfs_cached_write(inode_t *inode, const void *buf, size_t count, off_t pos)
{
    const uint8_t *in = (const uint8_t *)buf;
    size_t done = 0;

    if (!inode || !buf)
        return -EINVAL;

    while (done < count) {
        uint64_t index = ((uint64_t)pos + done) >> PAGE_SHIFT;
        size_t poff = (size_t)(((uint64_t)pos + done) & (PAGE_SIZE - 1));
        size_t take = PAGE_SIZE - poff;
        page_cache_entry_t *e;

        if (take > count - done)
            take = count - done;
        e = pc_alloc(inode, index);
        if (!e)
            return done ? (ssize_t)done : -ENOMEM;
        if (poff || take < PAGE_SIZE)
            pc_fill(inode, e);
        memcpy(e->data + poff, in + done, take);
        e->dirty = 1;
        if (inode->i_mapping && inode->i_mapping->a_ops &&
            inode->i_mapping->a_ops->writepage)
            inode->i_mapping->a_ops->writepage(inode, e->data, index);
        done += take;
    }
    if ((uint64_t)pos + done > inode->i_size)
        inode->i_size = (uint64_t)pos + done;
    return (ssize_t)done;
}

int vfs_cache_invalidate(inode_t *inode)
{
    size_t i;
    for (i = 0; i < PCACHE_MAX; i++) {
        if (g_pcache[i].used && g_pcache[i].inode == inode) {
            g_pcache[i].used = 0;
            g_pcache[i].dirty = 0;
            g_pcache[i].inode = NULL;
        }
    }
    if (inode && inode->i_mapping && inode->i_mapping->a_ops &&
        inode->i_mapping->a_ops->invalidate)
        return inode->i_mapping->a_ops->invalidate(inode);
    return 0;
}
