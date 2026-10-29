#pragma once
#include <stdint.h>

/* arm64 part of struct cpu. TPIDR_EL1 holds the struct cpu pointer itself. */
struct arch_cpu {
    uint64_t kernel_sp; /* stack top for exceptions taken from EL0 */
    uint64_t mpidr;
    volatile uint8_t *gicr; /* this CPU's GIC redistributor */
    uint32_t irq_intid;     /* interrupt being handled, for irq_eoi */
};

struct cpu;

static inline struct cpu *this_cpu(void) {
    struct cpu *c;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(c));
    return c;
}

static inline uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile("mrs %0, daif; msr daifset, #3" : "=r"(flags)::"memory");
    return flags;
}

static inline void irq_restore(uint64_t flags) { __asm__ volatile("msr daif, %0" ::"r"(flags) : "memory"); }

static inline void irq_enable(void) { __asm__ volatile("msr daifclr, #3" ::: "memory"); }
static inline void irq_disable(void) { __asm__ volatile("msr daifset, #3" ::: "memory"); }
static inline void cpu_relax(void) { __asm__ volatile("yield"); }
/* wfi wakes on a pending interrupt even while it is masked, so we unmask only
 * after it: an interrupt that arrives after the caller's check is never missed. */
static inline void cpu_idle_wait(void) { __asm__ volatile("wfi; msr daifclr, #3" ::: "memory"); }
static inline void halt(void) { __asm__ volatile("wfi"); }

/* Reading the pointer and then the field takes two loads, and a migration
 * between them would read another CPU's field, so we keep interrupts off. */
#define this_cpu_read(field)                                                                       \
    ({                                                                                             \
        uint64_t f_ = irq_save();                                                                  \
        __typeof__(((struct cpu *)0)->field) v_ = this_cpu()->field;                               \
        irq_restore(f_);                                                                           \
        v_;                                                                                        \
    })

static inline uint64_t arch_counter(void) {
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v)::"memory");
    return v;
}
