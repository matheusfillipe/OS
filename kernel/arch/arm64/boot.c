#include <kernel/arch.h>
#include <kernel/boot.h>
#include <kernel/lib.h>
#include <kernel/proc.h>

extern char exception_vectors[];

/* SCTLR_EL1 bits that let EL0 run what Linux programs expect: wfi and wfe,
 * dc zva, reading CTR_EL0, and cache maintenance for code they write. */
#define SCTLR_EL0_ACCESS (1ULL << 14 | 1ULL << 15 | 1ULL << 16 | 1ULL << 18 | 1ULL << 26)

void cpu_init(struct cpu *c) {
    __asm__ volatile("msr tpidr_el1, %0" ::"r"(c) : "memory");
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(c->arch.mpidr));
    /* Exceptions taken at EL1 must run on SP_EL1, so we move our stack there. */
    __asm__ volatile("mrs x9, spsel; cbnz x9, 1f; mov x9, sp; msr spsel, #1; mov sp, x9; 1:" ::: "x9", "memory");
    __asm__ volatile("msr vbar_el1, %0; isb" ::"r"(exception_vectors) : "memory");
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    __asm__ volatile("msr sctlr_el1, %0; isb" ::"r"(sctlr | SCTLR_EL0_ACCESS));
    fpu_init_cpu();
}

void arch_init_bsp(struct cpu *c) {
    cpu_init(c);
    kprintf("cpu: exception vectors loaded\n");
}

void arch_init_interrupts(struct cpu *c) {
    (void)c;
    acpi_init();
    gic_init();
}
