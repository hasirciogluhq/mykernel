#include <kernel/heap.h>

typedef struct heap_block {
    size_t size; /* bytes from this header to end of payload */
} heap_block_t;

static uint8_t *heap_base;
static uint8_t *heap_ptr;
static uint8_t *heap_end;

void heap_init(void *start, size_t size)
{
    heap_base = (uint8_t *)start;
    heap_ptr = heap_base;
    heap_end = heap_base + size;
}

void *kmalloc(size_t size)
{
    return kmalloc_aligned(size, 8);
}

void *kmalloc_aligned(size_t size, size_t align)
{
    if (size == 0 || align == 0)
        return NULL;
    if (align < 4)
        align = 4;

    uintptr_t cur = (uintptr_t)heap_ptr;
    uintptr_t data_addr = (cur + sizeof(heap_block_t) + (align - 1)) & ~(uintptr_t)(align - 1);
    heap_block_t *blk = (heap_block_t *)(data_addr - sizeof(heap_block_t));
    uint8_t *data = (uint8_t *)data_addr;

    if (data + size > heap_end)
        return NULL;

    blk->size = (size_t)((data + size) - (uint8_t *)blk);
    heap_ptr = data + size;
    return data;
}

void kfree(void *ptr)
{
    if (!ptr)
        return;

    heap_block_t *blk = (heap_block_t *)((uint8_t *)ptr - sizeof(heap_block_t));
    if ((uint8_t *)blk + blk->size != heap_ptr)
        return;
    heap_ptr = (uint8_t *)blk;
}

size_t heap_used(void)
{
    return (size_t)(heap_ptr - heap_base);
}

size_t heap_free(void)
{
    return (size_t)(heap_end - heap_ptr);
}
