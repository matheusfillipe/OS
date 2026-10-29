#include <kernel/arch.h>
#include <kernel/lib.h>
#include <kernel/proc.h>
#include <linux/abi.h>

/* ESR_EL1 exception classes. */
#define EC_FP_EXC64    0x2c
#define EC_SVC64       0x15
#define EC_IABT_LOW    0x20
#define EC_IABT_CUR    0x21
#define EC_PC_ALIGN    0x22
#define EC_DABT_LOW    0x24
#define EC_DABT_CUR    0x25
#define EC_SP_ALIGN    0x26
#define EC_BREAKPT_LOW 0x30
#define EC_SOFTSTP_LOW 0x32
#define EC_WATCHPT_LOW 0x34
#define EC_BRK64       0x3c

#define ESR_WNR (1ULL << 6)
#define ESR_CM  (1ULL << 8)

/* Vector table slots (entry.S): current EL on SP_ELx, and lower EL in AArch64. */
#define VECTOR_SYNC_EL1 4
#define VECTOR_IRQ_EL1  5
#define VECTOR_SYNC_EL0 8
#define VECTOR_IRQ_EL0  9

#define BRK_SELFTEST 1

void signal_deliver(struct regs *r);
void user_exception(struct regs *r, uint64_t vector, int sig);
bool page_fault(struct regs *r, uint64_t addr, bool write, bool exec);

static volatile int breakpoint_hits;

static void dump_regs(const struct regs *r) {
    kprintf("  pc=%016lx sp=%016lx pstate=%08lx\n", r->pc, r->sp, r->pstate);
    kprintf("  esr=%016lx far=%016lx\n", r->esr, r->far);
    for (int i = 0; i < 30; i += 2)
        kprintf("  x%d=%016lx x%d=%016lx\n", i, r->x[i], i + 1, r->x[i + 1]);
    kprintf("  x30=%016lx\n", r->x[30]);
}

/* Translation, access flag and permission faults: the ones page tables resolve. */
static bool is_mmu_fault(uint64_t esr) {
    uint64_t fsc = esr & 0x3f;
    return fsc >= 0x04 && fsc <= 0x0f;
}

static int exception_signal(uint64_t ec) {
    switch (ec) {
    case EC_FP_EXC64:
        return SIGFPE;
    case EC_PC_ALIGN:
    case EC_SP_ALIGN:
    case EC_IABT_LOW:
    case EC_DABT_LOW:
        return SIGBUS;
    case EC_BRK64:
    case EC_BREAKPT_LOW:
    case EC_SOFTSTP_LOW:
    case EC_WATCHPT_LOW:
        return SIGTRAP;
    default:
        return SIGILL;
    }
}

static void sync_exception(struct regs *r) {
    uint64_t ec = r->esr >> 26;
    bool user = regs_user_mode(r);
    if (ec == EC_SVC64 && user) {
        irq_enable();
        syscall_dispatch(r);
        return;
    }
    bool iabt = ec == EC_IABT_LOW || ec == EC_IABT_CUR, dabt = ec == EC_DABT_LOW || ec == EC_DABT_CUR;
    if ((iabt || dabt) && is_mmu_fault(r->esr)) {
        bool write = dabt && (r->esr & ESR_WNR) && !(r->esr & ESR_CM);
        if (page_fault(r, r->far, write, iabt)) {
            if (user)
                signal_deliver(r);
            return;
        }
        kprintf("\nkernel page fault at %lx, pc %lx, esr %lx\n", r->far, r->pc, r->esr);
        panic("page fault in kernel mode");
    }
    /* brk leaves pc on itself, so we step over it. */
    if (ec == EC_BRK64 && !user && (r->esr & 0xffff) == BRK_SELFTEST) {
        breakpoint_hits++;
        r->pc += 4;
        return;
    }
    if (user) {
        user_exception(r, ec, exception_signal(ec));
        return;
    }
    kprintf("\nunhandled exception class %lx\n", ec);
    dump_regs(r);
    panic("unhandled CPU exception");
}

void exception_dispatch(struct regs *r, uint64_t vector);
void exception_dispatch(struct regs *r, uint64_t vector) {
    switch (vector) {
    case VECTOR_SYNC_EL1:
    case VECTOR_SYNC_EL0:
        sync_exception(r);
        return;
    case VECTOR_IRQ_EL1:
    case VECTOR_IRQ_EL0:
        irq_dispatch(r);
        if (regs_user_mode(r))
            signal_deliver(r);
        return;
    }
    kprintf("\nunexpected exception vector %lu\n", vector);
    dump_regs(r);
    panic("unhandled CPU exception");
}

void arch_page_fault_init(void) {}

void arch_selftest_breakpoint(void) {
    __asm__ volatile("brk %0" ::"i"(BRK_SELFTEST));
    KASSERT(breakpoint_hits == 1);
    kprintf("selftest: interrupts ok\n");
}
