#include <kernel/lib.h>

/* GCC may emit calls to these even in freestanding code, so they must exist
 * with exactly these names. */

/* We move 8 bytes per step with rep movsq / stosq and finish the tail byte-wise.
 * The direction flag is clear because every kernel entry path executes cld. */
void *memset(void *dst, int c, size_t n) {
    uint64_t pattern = 0x0101010101010101ULL * (uint8_t)c;
    void *d = dst;
    size_t words = n / 8, tail = n % 8;
    __asm__ volatile("rep stosq" : "+D"(d), "+c"(words) : "a"(pattern) : "memory");
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(tail) : "a"(pattern) : "memory");
    return dst;
}

void *memcpy(void *restrict dst, const void *restrict src, size_t n) {
    void *d = dst;
    const void *s = src;
    size_t words = n / 8, tail = n % 8;
    __asm__ volatile("rep movsq" : "+D"(d), "+S"(s), "+c"(words) : : "memory");
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(tail) : : "memory");
    return dst;
}
