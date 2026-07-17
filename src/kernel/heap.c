#include <kernel/heap.h>
#include <kernel/spinlock.h>

/*
 * First-fit freelist + wilderness (bump) heap.
 *
 * Fresh memory comes from the wilderness bump pointer so a single freelist
 * metadata smash cannot orphan the entire arena. Freed blocks go on an
 * address-ordered freelist and are coalesced; a block that abuts the bump
 * pointer is returned to the wilderness instead.
 *
 * Every returned pointer has a real heap_block_t header immediately before
 * it (including high-alignment allocs). High alignment is done by carving
 * lead/tail scraps out of an oversized region — never by writing a fake
 * ALIGN tag into another block's payload/metadata.
 */

typedef struct heap_block {
    size_t size; /* total bytes including this header */
    uint32_t magic;
    struct heap_block *next; /* freelist link when FREE */
} heap_block_t;

#define HEAP_MAGIC_USED 0xA110C001u
#define HEAP_MAGIC_FREE 0xF4EEB10Cu
#define HEAP_HDR_SIZE   ((sizeof(heap_block_t) + 15u) & ~15u)
#define HEAP_MIN_BLOCK  (HEAP_HDR_SIZE + 16u)

static uint8_t *heap_base;
static uint8_t *heap_end;
static uint8_t *wilderness;
static heap_block_t *free_list;
static size_t g_used;
static spinlock_t g_heap_lock;

static void free_raw_locked_region(heap_block_t *blk);

static size_t align_up_sz(size_t n, size_t align)
{
    return (n + (align - 1u)) & ~(align - 1u);
}

static uintptr_t align_up_ptr(uintptr_t p, size_t align)
{
    return (p + (align - 1u)) & ~(uintptr_t)(align - 1u);
}

static int block_in_heap(const heap_block_t *b)
{
    const uint8_t *p = (const uint8_t *)b;
    return p >= heap_base && (uint8_t *)b + HEAP_HDR_SIZE <= heap_end;
}

static int block_span_ok(const heap_block_t *b)
{
    uint8_t *end;

    if (!block_in_heap(b) || b->size < HEAP_HDR_SIZE)
        return 0;
    end = (uint8_t *)b + b->size;
    if (end < (uint8_t *)b || end > heap_end)
        return 0;
    return 1;
}

static int size_add_ok(size_t a, size_t b, size_t *out)
{
    if (a > (size_t)-1 - b)
        return 0;
    *out = a + b;
    return 1;
}

static int try_return_wilderness(heap_block_t *b)
{
    uint8_t *end;

    if (!b || !block_span_ok(b))
        return 0;
    end = (uint8_t *)b + b->size;
    if (end != wilderness)
        return 0;
    wilderness = (uint8_t *)b;
    return 1;
}

static void freelist_detach(heap_block_t *b)
{
    heap_block_t **pp = &free_list;

    while (*pp) {
        if (*pp == b) {
            *pp = b->next;
            b->next = NULL;
            return;
        }
        pp = &(*pp)->next;
    }
}

static void freelist_insert(heap_block_t *b)
{
    heap_block_t **pp;
    heap_block_t *prev;
    heap_block_t *n;
    size_t sum;

    if (!b || !block_span_ok(b))
        return;

    if (try_return_wilderness(b))
        return;

    b->magic = HEAP_MAGIC_FREE;
    b->next = NULL;

    pp = &free_list;
    while (*pp && (uintptr_t)*pp < (uintptr_t)b)
        pp = &(*pp)->next;

    b->next = *pp;
    *pp = b;

    /* Coalesce forward (only with a verified FREE neighbor). */
    if (b->next && b->next->magic == HEAP_MAGIC_FREE &&
        (uint8_t *)b + b->size == (uint8_t *)b->next &&
        block_span_ok(b->next) &&
        size_add_ok(b->size, b->next->size, &sum)) {
        n = b->next;
        b->size = sum;
        b->next = n->next;
    }

    if (try_return_wilderness(b)) {
        freelist_detach(b);
        return;
    }

    /* Coalesce backward. */
    prev = NULL;
    for (n = free_list; n && n != b; n = n->next)
        prev = n;
    if (prev && prev->magic == HEAP_MAGIC_FREE &&
        (uint8_t *)prev + prev->size == (uint8_t *)b &&
        block_span_ok(prev) &&
        size_add_ok(prev->size, b->size, &sum)) {
        prev->size = sum;
        prev->next = b->next;
        if (try_return_wilderness(prev))
            freelist_detach(prev);
    }
}

static void freelist_remove(heap_block_t *b)
{
    freelist_detach(b);
}

static void split_tail(heap_block_t *b, size_t keep)
{
    heap_block_t *tail;
    size_t left;

    keep = align_up_sz(keep, 16u);
    if (keep < HEAP_HDR_SIZE)
        keep = HEAP_HDR_SIZE;
    if (b->size < keep + HEAP_MIN_BLOCK)
        return;

    left = b->size - keep;
    tail = (heap_block_t *)((uint8_t *)b + keep);
    tail->size = left;
    tail->magic = HEAP_MAGIC_FREE;
    tail->next = NULL;
    b->size = keep;
    freelist_insert(tail);
}

/* Release [start, end) as a free scrap (wilderness or freelist). */
static void release_span(uint8_t *start, uint8_t *end)
{
    heap_block_t *b;
    size_t sz;

    if (!start || !end || end <= start)
        return;
    sz = (size_t)(end - start);
    if (sz < HEAP_MIN_BLOCK)
        return; /* too small — absorbed by neighbor used block instead */
    if (((uintptr_t)start) & 15u)
        return;

    b = (heap_block_t *)start;
    b->size = sz;
    b->magic = HEAP_MAGIC_FREE;
    b->next = NULL;
    freelist_insert(b);
}

static void *bump_region(size_t need)
{
    uint8_t *next;

    need = align_up_sz(need, 16u);
    if (need < HEAP_MIN_BLOCK)
        need = HEAP_MIN_BLOCK;
    if (wilderness + need < wilderness || wilderness + need > heap_end)
        return NULL;

    next = wilderness + need;
    wilderness = next;
    return next - need;
}

/*
 * Take an owned contiguous region of exactly `need` bytes (16-aligned),
 * either by splitting a free block or bumping wilderness.
 */
static heap_block_t *take_region(size_t need)
{
    heap_block_t *b;
    uint8_t *region;

    need = align_up_sz(need, 16u);

    for (b = free_list; b; b = b->next) {
        if (b->magic != HEAP_MAGIC_FREE || !block_span_ok(b))
            continue;
        if (b->size < need || (((uintptr_t)b) & 15u))
            continue;

        freelist_remove(b);
        split_tail(b, need);
        b->magic = HEAP_MAGIC_USED;
        b->next = NULL;
        return b;
    }

    region = (uint8_t *)bump_region(need);
    if (!region)
        return NULL;

    b = (heap_block_t *)region;
    b->size = need;
    b->magic = HEAP_MAGIC_USED;
    b->next = NULL;
    return b;
}

static void *finish_used(heap_block_t *b)
{
    b->magic = HEAP_MAGIC_USED;
    b->next = NULL;
    g_used += b->size;
    return (uint8_t *)b + HEAP_HDR_SIZE;
}

/* Allocate `size` payload bytes with natural 16-byte payload alignment. */
static void *alloc_raw_locked(size_t size)
{
    heap_block_t *b;
    size_t need;

    size = align_up_sz(size, 16u);
    need = align_up_sz(HEAP_HDR_SIZE + size, 16u);
    b = take_region(need);
    if (!b)
        return NULL;
    return finish_used(b);
}

/*
 * High alignment: obtain an oversized region, place the real header
 * immediately before an aligned user pointer, return lead/tail scraps.
 */
static void *alloc_aligned_locked(size_t size, size_t align)
{
    heap_block_t *region;
    heap_block_t *hdr;
    uint8_t *reg_start;
    uint8_t *reg_end;
    uintptr_t user;
    size_t need;
    size_t used_sz;
    size_t lead;
    size_t tail;

    size = align_up_sz(size, 16u);
    /*
     * Worst case: HDR before the aligned user pointer, up to (align-1) bytes of
     * lead padding, plus HEAP_MIN_BLOCK slack so we can skip a sub-minimum lead
     * and still fit the next alignment slot.
     */
    if (!size_add_ok(HEAP_HDR_SIZE + size, align, &need))
        return NULL;
    if (!size_add_ok(need, HEAP_MIN_BLOCK, &need))
        return NULL;
    need = align_up_sz(need, 16u);

    region = take_region(need);
    if (!region)
        return NULL;

    reg_start = (uint8_t *)region;
    reg_end = reg_start + region->size;

    /*
     * Find an aligned user pointer such that [user - HDR, user + size)
     * fits in the region. If the lead scrap would be a non-empty but
     * sub-MIN_BLOCK fragment, bump to the next alignment boundary.
     */
    user = align_up_ptr((uintptr_t)reg_start + HEAP_HDR_SIZE, align);
    for (;;) {
        if (user < (uintptr_t)reg_start + HEAP_HDR_SIZE)
            goto fail;
        if (user + size < user || user + size > (uintptr_t)reg_end)
            goto fail;

        hdr = (heap_block_t *)(user - HEAP_HDR_SIZE);
        lead = (size_t)((uint8_t *)hdr - reg_start);
        used_sz = align_up_sz((size_t)((user + size) - (uintptr_t)hdr), 16u);
        if ((uint8_t *)hdr + used_sz < (uint8_t *)hdr ||
            (uint8_t *)hdr + used_sz > reg_end)
            goto fail;

        if (lead == 0 || lead >= HEAP_MIN_BLOCK)
            break;

        /* Lead too small to free — try next aligned slot. */
        if (user + align < user)
            goto fail;
        user += align;
    }

    tail = (size_t)(reg_end - ((uint8_t *)hdr + used_sz));
    /* Absorb a sub-minimum tail into the used block so it is not lost. */
    if (tail > 0 && tail < HEAP_MIN_BLOCK) {
        used_sz += tail;
        tail = 0;
    }

    /* Region was taken as one USED block; carve scraps back out. */
    if (lead >= HEAP_MIN_BLOCK) {
        region->size = lead;
        region->magic = HEAP_MAGIC_FREE;
        region->next = NULL;
        freelist_insert(region);
    }

    hdr->size = used_sz;
    hdr->magic = HEAP_MAGIC_USED;
    hdr->next = NULL;

    if (tail >= HEAP_MIN_BLOCK)
        release_span((uint8_t *)hdr + used_sz, reg_end);

    g_used += hdr->size;
    return (void *)user;

fail:
    /* Give the whole region back so it is not lost. */
    freelist_insert(region);
    return NULL;
}

static void free_raw_locked_region(heap_block_t *blk)
{
    if (!blk)
        return;
    if (g_used >= blk->size)
        g_used -= blk->size;
    else
        g_used = 0;
    freelist_insert(blk);
}

static void free_raw_locked(void *ptr)
{
    heap_block_t *blk;

    if (!ptr)
        return;

    blk = (heap_block_t *)((uint8_t *)ptr - HEAP_HDR_SIZE);
    if (!block_span_ok(blk) || blk->magic != HEAP_MAGIC_USED)
        return;

    free_raw_locked_region(blk);
}

void heap_init(void *start, size_t size)
{
    uintptr_t s;
    uintptr_t e;

    spin_init(&g_heap_lock);
    free_list = NULL;
    g_used = 0;
    heap_base = NULL;
    heap_end = NULL;
    wilderness = NULL;

    if (!start || size < HEAP_MIN_BLOCK)
        return;

    /* Keep 16-byte geometry for headers and splits. */
    s = align_up_ptr((uintptr_t)start, 16u);
    e = ((uintptr_t)start + size) & ~(uintptr_t)15u;
    if (e < s + HEAP_MIN_BLOCK)
        return;

    heap_base = (uint8_t *)s;
    heap_end = (uint8_t *)e;
    wilderness = heap_base;
}

void *kmalloc(size_t size)
{
    return kmalloc_aligned(size, 8);
}

void *kmalloc_aligned(size_t size, size_t align)
{
    void *out;
    uint32_t flags;

    if (size == 0 || align == 0)
        return NULL;
    if (align < 4)
        align = 4;
    if (align & (align - 1u))
        return NULL;

    flags = spin_lock_irqsave(&g_heap_lock);

    if (align <= 16)
        out = alloc_raw_locked(size);
    else
        out = alloc_aligned_locked(size, align);

    spin_unlock_irqrestore(&g_heap_lock, flags);
    return out;
}

void kfree(void *ptr)
{
    uint32_t flags;

    if (!ptr)
        return;

    flags = spin_lock_irqsave(&g_heap_lock);
    free_raw_locked(ptr);
    spin_unlock_irqrestore(&g_heap_lock, flags);
}

size_t heap_used(void)
{
    size_t n;
    uint32_t flags = spin_lock_irqsave(&g_heap_lock);
    n = g_used;
    spin_unlock_irqrestore(&g_heap_lock, flags);
    return n;
}

size_t heap_free(void)
{
    size_t n;
    heap_block_t *b;
    uint32_t flags;

    flags = spin_lock_irqsave(&g_heap_lock);
    n = 0;
    if (heap_end > wilderness)
        n += (size_t)(heap_end - wilderness);
    for (b = free_list; b; b = b->next) {
        if (b->magic == HEAP_MAGIC_FREE && block_span_ok(b) &&
            b->size > HEAP_HDR_SIZE)
            n += b->size - HEAP_HDR_SIZE;
    }
    spin_unlock_irqrestore(&g_heap_lock, flags);
    return n;
}
