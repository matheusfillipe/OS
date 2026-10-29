#include <kernel/lib.h>

/* GCC may emit calls to these even in freestanding code, so they must exist
 * with exactly these names. It would also turn these very loops back into
 * calls to themselves, hence the optimize attribute. We move 8 bytes per step
 * when both pointers allow it and finish the tail byte-wise. */

__attribute__((optimize("no-tree-loop-distribute-patterns"))) void *memset(void *dst, int c, size_t n) {
    uint8_t *d = dst;
    uint64_t pattern = 0x0101010101010101ULL * (uint8_t)c;
    for (; n && ((uint64_t)d & 7); n--)
        *d++ = (uint8_t)c;
    for (; n >= 8; n -= 8, d += 8)
        *(uint64_t *)d = pattern;
    while (n--)
        *d++ = (uint8_t)c;
    return dst;
}

__attribute__((optimize("no-tree-loop-distribute-patterns"))) void *memcpy(void *restrict dst,
                                                                          const void *restrict src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    if ((((uint64_t)d ^ (uint64_t)s) & 7) == 0) {
        for (; n && ((uint64_t)d & 7); n--)
            *d++ = *s++;
        for (; n >= 8; n -= 8, d += 8, s += 8)
            *(uint64_t *)d = *(const uint64_t *)s;
    }
    while (n--)
        *d++ = *s++;
    return dst;
}
