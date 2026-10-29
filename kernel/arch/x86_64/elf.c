#include <asm/msr.h>
#include <kernel/arch.h>

uint64_t arch_hwcap(void) {
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    return d;
}
