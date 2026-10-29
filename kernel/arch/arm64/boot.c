#include <kernel/arch.h>
#include <kernel/boot.h>
#include <kernel/lib.h>

extern char exception_vectors[];

void arch_init_bsp(struct cpu *c) {
    __asm__ volatile("msr tpidr_el1, %0" ::"r"(c) : "memory");
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(c->arch.mpidr));
    /* Exceptions taken at EL1 must run on SP_EL1, so we move our stack there. */
    __asm__ volatile("mrs x9, spsel; cbnz x9, 1f; mov x9, sp; msr spsel, #1; mov sp, x9; 1:" ::: "x9", "memory");
    __asm__ volatile("msr vbar_el1, %0; isb" ::"r"(exception_vectors) : "memory");
    kprintf("cpu: exception vectors loaded\n");
}

void arch_init_interrupts(struct cpu *c) {
    (void)c;
    acpi_init();
    gic_init();
}
