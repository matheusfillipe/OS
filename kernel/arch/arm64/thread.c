#include <kernel/arch.h>
#include <kernel/lib.h>
#include <kernel/proc.h>

void thread_start(void);

/* Initial frame consumed by switch_context: x19-x29 zero, then x30, which
 * its ret jumps to. */
uint64_t arch_thread_initial_sp(uint64_t stack_top) {
    uint64_t *sp = (uint64_t *)stack_top - 12;
    memset(sp, 0, 12 * 8);
    sp[11] = (uint64_t)thread_start;
    return (uint64_t)sp;
}

void return_to_user(struct regs *frame) {
    (void)frame;
    panic("arm64: return to EL0 is not implemented yet");
}

void arch_set_kernel_stack(struct cpu *c, uint64_t top) { c->arch.kernel_sp = top; }

void arch_load_tls(uint64_t tls) { __asm__ volatile("msr tpidr_el0, %0" ::"r"(tls)); }

void arch_exec_init_regs(struct regs *r, uint64_t entry, uint64_t sp) {
    memset(r, 0, sizeof *r);
    r->pc = entry;
    r->sp = sp;
}

void arch_fork_child_regs(struct regs *r, uint64_t new_sp) {
    r->x[0] = 0;
    if (new_sp)
        r->sp = new_sp;
}
