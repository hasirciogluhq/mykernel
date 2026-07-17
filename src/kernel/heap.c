#include <kernel/heap.h>
#include <kernel/spinlock.h>

typedef struct heap_block {
    size_t size; /* bytes from this header to end of payload */
} heap_block_t;

static uint8_t *heap_base;
static uint8_t *heap_ptr;
static uint8_t *heap_end;
static spinlock_t g_heap_lock;

void heap_init(void *start, size_t size)
{
    heap_base = (uint8_t *)start;
    heap_ptr = heap_base;
    heap_end = heap_base + size;
    spin_init(&g_heap_lock);
}

void *kmalloc(size_t size)
{
    return kmalloc_aligned(size, 8);
}

void *kmalloc_aligned(size_t size, size_t align)
{
    uintptr_t cur;
    uintptr_t data_addr;
    heap_block_t *blk;
    uint8_t *data;

    if (size == 0 || align == 0)
        return NULL;
    if (align < 4)
        align = 4;

    spin_lock(&g_heap_lock);
    cur = (uintptr_t)heap_ptr;
    data_addr = (cur + sizeof(heap_block_t) + (align - 1)) & ~(uintptr_t)(align - 1);
    blk = (heap_block_t *)(data_addr - sizeof(heap_block_t));
    data = (uint8_t *)data_addr;

    if (data + size > heap_end) {
        spin_unlock(&g_heap_lock);
        return NULL;
    }

    blk->size = (size_t)((data + size) - (uint8_t *)blk);
    heap_ptr = data + size;
    spin_unlock(&g_heap_lock);
    return data;
}

void kfree(void *ptr)
{
    heap_block_t *blk;

    if (!ptr)
        return;

    spin_lock(&g_heap_lock);
    blk = (heap_block_t *)((uint8_t *)ptr - sizeof(heap_block_t));
    if ((uint8_t *)blk + blk->size == heap_ptr)
        heap_ptr = (uint8_t *)blk;
    spin_unlock(&g_heap_lock);
}

size_t heap_used(void)
{
    size_t n;
    spin_lock(&g_heap_lock);
    n = (size_t)(heap_ptr - heap_base);
    spin_unlock(&g_heap_lock);
    return n;
}

size_t heap_free(void)
{
    size_t n;
    spin_lock(&g_heap_lock);
    n = (size_t)(heap_end - heap_ptr);
    spin_unlock(&g_heap_lock);
    return n;
}
