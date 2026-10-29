#include <kernel/arch.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/proc.h>
#include <kernel/sched.h>

/* The scheduler tick comes from each CPU's EL1 virtual timer. */

static uint64_t timer_period; /* counter ticks per 1/HZ s */

void arch_send_resched(struct cpu *c) { gic_send_sgi(c, VEC_RESCHED); }

/* The timer's interrupt is level-triggered: we move the compare value past
 * the counter before the EOI, or the line would still be up. */
static void timer_irq(struct regs *r) {
    uint64_t cval;
    __asm__ volatile("mrs %0, cntv_cval_el0" : "=r"(cval));
    __asm__ volatile("msr cntv_cval_el0, %0; isb" ::"r"(cval + timer_period));
    irq_eoi();
    proc_account_tick(r); /* CPU times for /proc */
    sched_tick();
}

void sched_preempt(void);

static void resched_irq(struct regs *r) {
    (void)r;
    irq_eoi();
    sched_preempt();
}

static void timer_init_cpu(void) {
    __asm__ volatile("msr cntv_cval_el0, %0; msr cntv_ctl_el0, %1; isb" ::"r"(arch_counter() + timer_period),
                     "r"(1UL));
}

/* Every TLB invalidation we issue is broadcast to the inner shareable domain
 * and complete at its dsb ish, so other CPUs need no IPI to drop entries. */
volatile int tlb_shootdowns_active;

void tlb_service(void) {
    struct cpu *c = this_cpu();
    /* Clear first, then flush: a request that arrives after the clear stays
     * pending (and has its own IPI), so none is lost. */
    if (__atomic_exchange_n(&c->tlb_flush_pending, 0, __ATOMIC_ACQ_REL))
        arch_tlb_flush_all();
}

void tlb_shootdown(struct mm *mm) { (void)mm; }

void smp_init(void) {
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    timer_period = freq / HZ;
    irq_set_handler(VEC_TIMER, timer_irq);
    irq_set_handler(VEC_RESCHED, resched_irq);
    gic_init_cpu();
    timer_init_cpu(); /* BSP tick */
    this_cpu()->online = true;
    kprintf("smp: timer %lu kHz; starting secondary CPUs is not implemented yet, running on one core\n",
            freq / 1000);
}
