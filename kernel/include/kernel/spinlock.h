#pragma once
#include <asm/cpu.h>
#include <stdint.h>

typedef struct {
    volatile int locked;
} spinlock_t;

#define SPINLOCK_INIT {0}

static inline void spin_lock(spinlock_t *l) {
    while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&l->locked, __ATOMIC_RELAXED))
            arch_spin_wait();
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
