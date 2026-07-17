#ifndef MYKERNEL_SPINLOCK_H
#define MYKERNEL_SPINLOCK_H

#include <kernel/types.h>

typedef struct spinlock {
    volatile uint32_t locked;
} spinlock_t;

static inline void spin_init(spinlock_t *l)
{
    if (l)
        l->locked = 0;
}

static inline void spin_lock(spinlock_t *l)
{
    if (!l)
        return;
    for (;;) {
        if (__sync_bool_compare_and_swap(&l->locked, 0, 1))
            break;
        __asm__ volatile("pause" ::: "memory");
    }
}

static inline void spin_unlock(spinlock_t *l)
{
    if (!l)
        return;
    __asm__ volatile("" ::: "memory");
    l->locked = 0;
}

/* Acquire lock with interrupts disabled on this CPU. Returns previous FLAGS. */
static inline uint32_t spin_lock_irqsave(spinlock_t *l)
{
    uint32_t flags;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags) :: "memory");
    spin_lock(l);
    return flags;
}

static inline void spin_unlock_irqrestore(spinlock_t *l, uint32_t flags)
{
    spin_unlock(l);
    if (flags & 0x200)
        __asm__ volatile("sti" ::: "memory");
}

#endif
