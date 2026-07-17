#include <kernel/types.h>

/*
 * Minimal unsigned 64-bit divide/modulo for i386 freestanding kmods.
 * Do not use 64-bit / or % here — GCC would emit calls back into these helpers.
 */

uint64_t __udivmoddi4(uint64_t n, uint64_t d, uint64_t *rem)
{
    uint64_t q = 0;
    uint64_t bit = 1;

    if (d == 0)
        return 0;

    while ((d & 0x8000000000000000ULL) == 0) {
        if (d > n)
            break;
        d <<= 1;
        bit <<= 1;
    }

    while (bit) {
        if (d <= n) {
            n -= d;
            q |= bit;
        }
        d >>= 1;
        bit >>= 1;
    }

    if (rem)
        *rem = n;
    return q;
}

uint64_t __udivdi3(uint64_t n, uint64_t d)
{
    return __udivmoddi4(n, d, NULL);
}

uint64_t __umoddi3(uint64_t n, uint64_t d)
{
    uint64_t r = 0;
    __udivmoddi4(n, d, &r);
    return r;
}
