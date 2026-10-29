#include <kernel/arch.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/proc.h>
#include <linux/abi.h>

/* Linux arm64 signal frame (what rt_sigreturn, unwinders and debuggers expect):
 * siginfo, then the ucontext, whose uc_mcontext.__reserved holds an
 * fpsimd_context record and a null record that ends the list. */
#define FPSIMD_MAGIC  0x46508001
#define RESERVED_SIZE 4096

struct sigcontext {
    uint64_t fault_address;
    uint64_t regs[31];
    uint64_t sp, pc, pstate;
};
struct ucontext {
    uint64_t uc_flags, uc_link;
    uint64_t ss_sp;
    int32_t ss_flags, pad;
    uint64_t ss_size;
    uint64_t sigmask;
    uint8_t unused[128];
    struct sigcontext mc;
};
struct sigframe {
    struct siginfo info;
    struct ucontext uc;
    struct fpsimd_context fpsimd __attribute__((aligned(16)));
    uint32_t end[2];
};
#define SIGFRAME_SIZE (offsetof(struct sigframe, fpsimd) + RESERVED_SIZE)
_Static_assert(offsetof(struct sigframe, fpsimd) == 128 + 176 + 288, "Linux places __reserved here");

#define PSTATE_NZCV 0xf0000000ULL

/* glibc leaves SA_RESTORER unset on arm64 and returns from handlers through
 * the vDSO's trampoline. We have no vDSO, so every image gets a page with the
 * same two instructions, which unwinders recognise as a signal return. */
#define SIGRETURN_PAGE (USER_STACK_TOP + PAGE_SIZE)

static const uint32_t sigreturn_code[] = {
    0xd2801168, /* mov x8, #139 (rt_sigreturn) */
    0xd4000001, /* svc #0 */
};

void sigreturn_map(struct mm *mm);
void sigreturn_map(struct mm *mm) {
    if (mm_map(mm, SIGRETURN_PAGE, SIGRETURN_PAGE + PAGE_SIZE, PROT_READ | PROT_EXEC, VMA_ANON) == 0)
        mm_copy_out(mm, SIGRETURN_PAGE, sigreturn_code, sizeof sigreturn_code);
}

bool arch_signal_setup(struct regs *r, const struct siginfo *si, uint64_t mask, const struct linux_sigaction *sa) {
    struct user_thread *ut = thread_current()->user;

    /* Like Linux: a frame record {x29, x30} on top, so unwinders can walk out
     * of the handler, and the signal frame right below it. */
    uint64_t record = ALIGN_DOWN(r->sp, 16) - 16;
    uint64_t sp = ALIGN_DOWN(record - SIGFRAME_SIZE, 16);

    struct sigframe f = {.info = *si};
    f.uc.sigmask = mask;
    memcpy(f.uc.mc.regs, r->x, sizeof r->x);
    f.uc.mc.sp = r->sp;
    f.uc.mc.pc = r->pc;
    f.uc.mc.pstate = r->pstate;
    fpu_save(ut->fpu);
    memcpy(&f.fpsimd, ut->fpu, sizeof f.fpsimd);
    f.fpsimd.magic = FPSIMD_MAGIC;
    f.fpsimd.size = sizeof f.fpsimd;
    uint64_t frame_record[2] = {r->x[29], r->x[30]};
    if (copy_to_user(sp, &f, sizeof f) < 0 || copy_to_user(record, frame_record, sizeof frame_record) < 0)
        return false;

    r->x[0] = (uint64_t)si->si_signo;
    r->x[1] = sp + offsetof(struct sigframe, info);
    r->x[2] = sp + offsetof(struct sigframe, uc);
    r->x[29] = record;
    r->x[30] = sa->flags & SA_RESTORER ? sa->restorer : SIGRETURN_PAGE;
    r->sp = sp;
    r->pc = sa->handler;
    return true;
}

bool arch_signal_return(struct regs *r, uint64_t *mask) {
    struct user_thread *ut = thread_current()->user;
    /* The handler returned through the trampoline, so sp is at the frame again. */
    struct sigframe f;
    if ((r->sp & 15) || copy_from_user(&f, r->sp, sizeof f) < 0)
        return false;
    memcpy(r->x, f.uc.mc.regs, sizeof r->x);
    r->sp = f.uc.mc.sp;
    r->pc = f.uc.mc.pc;
    r->pstate = (r->pstate & ~PSTATE_NZCV) | (f.uc.mc.pstate & PSTATE_NZCV); /* user-changeable flags only */
    *mask = f.uc.sigmask;
    if (f.fpsimd.magic == FPSIMD_MAGIC && f.fpsimd.size == sizeof f.fpsimd) {
        memcpy(ut->fpu, &f.fpsimd, sizeof f.fpsimd);
        fpu_restore(ut->fpu);
    }
    return true;
}
