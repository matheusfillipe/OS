#pragma once
#include <stdint.h>

#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA   0x1b /* 0x18 | RPL3; data before code for SYSRET */
#define GDT_USER_CODE   0x23 /* 0x20 | RPL3 */
#define GDT_TSS         0x28

#define IST_DOUBLE_FAULT 1

struct tss {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

/* x86 part of struct cpu. Field order is fixed: syscall.S and this_cpu()
 * reach `self`, `kernel_rsp` and `user_rsp` through fixed %gs offsets. */
struct arch_cpu {
    struct cpu *self;     /* offset 0 */
    uint64_t kernel_rsp;  /* offset 8: stack top for syscall entry */
    uint64_t user_rsp;    /* offset 16: scratch for syscall entry */
    uint32_t lapic_id;
    uint64_t gdt[7];
    struct tss tss;
    uint8_t *df_stack;
    volatile int tlb_flush_pending; /* set by tlb_shootdown, cleared by the flush */
};

struct cpu;

void gdt_init(struct cpu *c);
void cpu_set_percpu(struct cpu *c);
void cpu_enable_features(void);
void syscall_init_cpu(void);

static inline struct cpu *this_cpu(void) {
    struct cpu *c;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(c));
    return c;
}

/* One gs-relative load: this_cpu()->field would be two loads, and a preemption
 * between them reads another CPU's field. */
#define this_cpu_read(field)                                                             \
    ({                                                                                   \
        __typeof__(((struct cpu *)0)->field) v_;                                         \
        __asm__ volatile("mov %%gs:%c1, %0"                                              \
                         : "=r"(v_)                                                      \
                         : "i"(__builtin_offsetof(struct cpu, field)));                  \
        v_;                                                                              \
    })

static inline uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags)::"memory");
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    if (flags & 0x200)
        __asm__ volatile("sti" ::: "memory");
}

static inline void irq_enable(void) { __asm__ volatile("sti"); }
static inline void irq_disable(void) { __asm__ volatile("cli"); }
static inline void cpu_relax(void) { __asm__ volatile("pause"); }
static inline void cpu_idle_wait(void) { __asm__ volatile("sti; hlt"); }
static inline void halt(void) { __asm__ volatile("hlt"); }

/* A CPU spinning on a lock usually has interrupts off, so it cannot take a TLB
 * shootdown IPI, while the CPU that sent the IPI waits for the answer and may
 * hold (or wait behind a holder of) the lock we want. To keep that from
 * deadlocking, spinners answer pending shootdowns themselves (smp.c). */
extern volatile int tlb_shootdowns_active;
void tlb_service(void);

static inline void arch_spin_wait(void) {
    cpu_relax();
    if (__atomic_load_n(&tlb_shootdowns_active, __ATOMIC_RELAXED))
        tlb_service();
}

static inline uint64_t arch_counter(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
