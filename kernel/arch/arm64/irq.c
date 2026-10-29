#include <kernel/arch.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/spinlock.h>

void irq_set_handler(unsigned irq, irq_handler_t handler) {
    (void)irq, (void)handler;
    panic("arm64: GIC interrupt routing is not implemented yet");
}

void irq_eoi(void) { panic("arm64: GIC end of interrupt is not implemented yet"); }

bool arch_msi_setup(const struct pci_dev *d, unsigned irq, uint64_t *addr, uint32_t *data) {
    (void)d, (void)irq, (void)addr, (void)data;
    panic("arm64: MSI through the GIC ITS is not implemented yet");
}

void arch_send_resched(struct cpu *c) {
    (void)c;
    panic("arm64: reschedule IPIs are not implemented yet");
}

void smp_init(void) { panic("arm64: starting secondary CPUs is not implemented yet"); }

volatile int tlb_shootdowns_active;

void tlb_service(void) {
    struct cpu *c = this_cpu();
    /* Clear first, then flush: a request that arrives after the clear stays
     * pending (and has its own IPI), so none is lost. */
    if (__atomic_exchange_n(&c->tlb_flush_pending, 0, __ATOMIC_ACQ_REL))
        arch_tlb_flush_all();
}

void tlb_shootdown(struct mm *mm) {
    (void)mm;
    if (cpu_count > 1)
        panic("arm64: TLB shootdown IPIs are not implemented yet");
}
