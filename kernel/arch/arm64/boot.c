#include <kernel/arch.h>
#include <kernel/lib.h>

void arch_init_bsp(struct cpu *c) {
    __asm__ volatile("msr tpidr_el1, %0" ::"r"(c) : "memory");
    panic("arm64: exception vectors are not implemented yet");
}

void arch_init_interrupts(struct cpu *c) {
    (void)c;
    panic("arm64: GIC and timer are not implemented yet");
}

void arch_selftest_breakpoint(void) { panic("arm64: breakpoint exceptions are not implemented yet"); }
