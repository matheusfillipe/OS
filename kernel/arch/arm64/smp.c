#include <kernel/arch.h>
#include <kernel/boot.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/proc.h>
#include <kernel/sched.h>

/* The scheduler tick comes from each CPU's EL1 virtual timer. */

__attribute__((used, section(".limine_requests")))
static volatile struct limine_mp_request boot_mp_request = {.id = LIMINE_MP_REQUEST_ID, .revision = 0};

static uint64_t timer_period; /* counter ticks per 1/HZ s */

void arch_send_resched(struct cpu *c) { gic_send_sgi(c, IRQ_RESCHED); }

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
 * and complete at its dsb ish, so other CPUs need no IPI to drop entries:
 * generic code invalidates each entry it changes (all of them, in mm_clone,
 * which runs on a CPU that has the mm loaded) before it calls
 * tlb_shootdown. Root switches flush only the local TLB, which is all they
 * have to. */
void tlb_shootdown(struct mm *mm) { (void)mm; }

/* Each secondary CPU arrives here on Limine's page tables and a small
 * bootloader stack, which becomes its idle thread's stack. */
static void ap_entry(struct limine_mp_info *info) {
    struct cpu *c = (struct cpu *)info->extra_argument;
    cpu_init(c);
    arch_mmu_init();
    arch_load_root(virt_to_phys_hhdm(vmm_kernel_pgd()));
    gic_init_cpu();
    timer_init_cpu();
    sched_init_cpu();
    __atomic_store_n(&c->online, true, __ATOMIC_RELEASE);
    for (;;)
        cpu_idle_wait();
}

void smp_init(void) {
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    timer_period = freq / HZ;
    irq_set_handler(IRQ_TIMER, timer_irq);
    irq_set_handler(IRQ_RESCHED, resched_irq);
    gic_init_cpu();
    timer_init_cpu(); /* BSP tick */
    this_cpu()->online = true;

    struct limine_mp_response *mp = boot_mp_request.response;
    if (!mp) {
        kprintf("smp: timer %lu kHz; bootloader gave no CPU list, running on one core\n", freq / 1000);
        return;
    }
    for (uint64_t i = 0; i < mp->cpu_count && i < MAX_CPUS; i++) {
        struct limine_mp_info *info = mp->cpus[i];
        if (info->mpidr == mp->bsp_mpidr)
            continue;
        struct cpu *c = kzalloc(sizeof *c);
        KASSERT(c);
        c->id = cpu_count;
        c->arch.mpidr = info->mpidr;
        cpus[cpu_count++] = c;
        info->extra_argument = (uint64_t)c;
        __atomic_store_n(&info->goto_address, ap_entry, __ATOMIC_SEQ_CST);
    }
    uint64_t deadline = global_ticks + HZ;
    for (uint32_t i = 1; i < cpu_count; i++)
        while (!__atomic_load_n(&cpus[i]->online, __ATOMIC_ACQUIRE)) {
            if (global_ticks > deadline)
                panic("smp: cpu %u (mpidr %lx) did not come online", i, cpus[i]->arch.mpidr);
            irq_enable();
            cpu_relax();
        }
    kprintf("smp: timer %lu kHz, %u cpu(s) online\n", freq / 1000, cpu_count);
}
