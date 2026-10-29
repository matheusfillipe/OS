#pragma once
#include <asm/cpu.h>
#include <stdint.h>

typedef struct {
    volatile int locked;
} spinlock_t;

#define SPINLOCK_INIT {0}

/* A CPU spinning here usually has interrupts off, so it cannot take a TLB
 * shootdown IPI, while the CPU that sent the IPI waits for the answer and may
 * hold (or wait behind a holder of) the lock we want. To keep that from
 * deadlocking, spinners answer pending shootdowns themselves (smp.c). */
extern volatile int tlb_shootdowns_active;
void tlb_service(void);

static inline void spin_lock(spinlock_t *l) {
    while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&l->locked, __ATOMIC_RELAXED)) {
            cpu_relax();
            if (__atomic_load_n(&tlb_shootdowns_active, __ATOMIC_RELAXED))
                tlb_service();
        }
}

static inline void spin_unlock(spinlock_t *l) { __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE); }

/* Lock that also keeps local interrupts off while held, so an interrupt
 * handler on the same CPU can never deadlock against its own thread. */
static inline uint64_t spin_lock_irqsave(spinlock_t *l) {
    uint64_t f = irq_save();
    spin_lock(l);
    return f;
}

static inline void spin_unlock_irqrestore(spinlock_t *l, uint64_t f) {
    spin_unlock(l);
    irq_restore(f);
}
