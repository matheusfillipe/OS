#include <kernel/arch.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/proc.h>
#include <linux/abi.h>

/* Linux x86-64 signal frame (what rt_sigreturn and debuggers expect). */
struct sigcontext {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15, rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags;
    uint16_t cs, gs, fs, ss;
    uint64_t err, trapno, oldmask, cr2, fpstate, reserved[8];
};
struct ucontext {
    uint64_t uc_flags, uc_link;
    uint64_t ss_sp;
    int32_t ss_flags, pad;
    uint64_t ss_size;
    struct sigcontext mc;
    uint64_t sigmask;
};
#define FPU_FRAME_SIZE 4096 /* generous: covers AVX-512 XSAVE areas */

bool arch_signal_setup(struct regs *r, const struct siginfo *si, uint64_t mask, const struct linux_sigaction *sa) {
    struct user_thread *ut = thread_current()->user;

    /* Linux layout, growing down from below the red zone: XSAVE area, then
     * {return address -> restorer, ucontext, siginfo}. At handler entry
     * (rsp + 8) is 16-byte aligned, as if the handler had been called. */
    uint64_t fpstate = ALIGN_DOWN(r->rsp - 128 - FPU_FRAME_SIZE, 64);
    uint64_t frame_size = 8 + sizeof(struct ucontext) + sizeof(struct siginfo);
    uint64_t sp = ALIGN_DOWN(fpstate - frame_size, 16) - 8;
    uint64_t uc_addr = sp + 8;
    uint64_t si_addr = uc_addr + sizeof(struct ucontext);

    struct ucontext uc = {0};
    uc.mc = (struct sigcontext){
        .r8 = r->r8, .r9 = r->r9, .r10 = r->r10, .r11 = r->r11, .r12 = r->r12, .r13 = r->r13,
        .r14 = r->r14, .r15 = r->r15, .rdi = r->rdi, .rsi = r->rsi, .rbp = r->rbp, .rbx = r->rbx,
        .rdx = r->rdx, .rax = r->rax, .rcx = r->rcx, .rsp = r->rsp, .rip = r->rip, .eflags = r->rflags,
        .cs = (uint16_t)r->cs, .ss = (uint16_t)r->ss, .fpstate = fpstate,
    };
    uc.sigmask = mask;
    uc.mc.oldmask = mask;
    uint64_t restorer = sa->restorer;

    fpu_save(ut->fpu);
    if (!(sa->flags & SA_RESTORER) || copy_to_user(fpstate, ut->fpu, FPU_FRAME_SIZE) < 0 ||
        copy_to_user(uc_addr, &uc, sizeof uc) < 0 || copy_to_user(si_addr, si, sizeof *si) < 0 ||
        copy_to_user(sp, &restorer, 8) < 0)
        return false;

    r->rip = sa->handler;
    r->rsp = sp;
    r->rdi = (uint64_t)si->si_signo;
    r->rsi = si_addr;
    r->rdx = uc_addr;
    r->rax = 0;
    r->rflags &= ~0x400ULL; /* DF clear per ABI */
    return true;
}

bool arch_signal_return(struct regs *r, uint64_t *mask) {
    struct user_thread *ut = thread_current()->user;
    /* The handler's `ret` popped the return address, so rsp is at the
     * ucontext the delivery code wrote. */
    struct ucontext uc;
    if (copy_from_user(&uc, r->rsp, sizeof uc) < 0)
        return false;
    struct sigcontext *m = &uc.mc;
    r->r8 = m->r8, r->r9 = m->r9, r->r10 = m->r10, r->r11 = m->r11, r->r12 = m->r12;
    r->r13 = m->r13, r->r14 = m->r14, r->r15 = m->r15, r->rdi = m->rdi, r->rsi = m->rsi;
    r->rbp = m->rbp, r->rbx = m->rbx, r->rdx = m->rdx, r->rax = m->rax, r->rcx = m->rcx;
    r->rsp = m->rsp, r->rip = m->rip;
    r->rflags = (r->rflags & ~0x40dd5ULL) | (m->eflags & 0x40dd5ULL); /* user-changeable flags only */
    *mask = uc.sigmask;
    if (m->fpstate && copy_from_user(ut->fpu, m->fpstate, 512) == 0) {
        /* Restore at least the legacy area; full XSAVE restore needs header validation. */
        fpu_restore(ut->fpu);
    }
    return true;
}
