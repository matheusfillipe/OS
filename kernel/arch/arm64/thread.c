#include <kernel/arch.h>
#include <kernel/lib.h>
#include <kernel/proc.h>

void switch_context(uint64_t *save_sp, uint64_t new_sp) {
    (void)save_sp, (void)new_sp;
    panic("arm64: context switch is not implemented yet");
}

uint64_t arch_thread_initial_sp(uint64_t stack_top) {
    (void)stack_top;
    panic("arm64: context switch is not implemented yet");
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
