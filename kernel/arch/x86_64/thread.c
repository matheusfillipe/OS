#include <asm/msr.h>
#include <kernel/arch.h>
#include <kernel/lib.h>
#include <kernel/proc.h>
#include <linux/abi.h>
#include <linux/syscall.h>

#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

void thread_start_trampoline(void);

/* Initial frame consumed by switch_context: six callee-saved registers,
 * then the return address. After the ret, rsp is 16-byte aligned, so the
 * trampoline's call enters thread_start with ABI-correct alignment. */
uint64_t arch_thread_initial_sp(uint64_t stack_top) {
    uint64_t *sp = (uint64_t *)stack_top;
    *--sp = (uint64_t)thread_start_trampoline;
    for (int i = 0; i < 6; i++)
        *--sp = 0;
    return (uint64_t)sp;
}

void arch_set_kernel_stack(struct cpu *c, uint64_t top) {
    c->arch.kernel_rsp = top;
    c->arch.tss.rsp[0] = top;
}

void arch_load_tls(uint64_t tls) { wrmsr(MSR_FS_BASE, tls); }

void arch_exec_init_regs(struct regs *r, uint64_t entry, uint64_t sp) {
    memset(r, 0, sizeof *r);
    r->rip = entry;
    r->rsp = sp;
    r->cs = GDT_USER_CODE;
    r->ss = GDT_USER_DATA;
    r->rflags = 0x202;
}

void arch_fork_child_regs(struct regs *r, uint64_t new_sp) {
    r->rax = 0;
    if (new_sp)
        r->rsp = new_sp;
}

SYSCALL(arch_prctl) {
    UNUSED_ARGS();
    struct user_thread *u = thread_current()->user;
    switch (a1) {
    case ARCH_SET_FS:
        if (a2 >= USER_TOP)
            return -EPERM;
        u->tls = a2;
        arch_load_tls(a2);
        return 0;
    case ARCH_GET_FS:
        return copy_to_user(a2, &u->tls, 8);
    case ARCH_SET_GS:
    case ARCH_GET_GS:
        return -EINVAL;
    }
    return -EINVAL;
}
