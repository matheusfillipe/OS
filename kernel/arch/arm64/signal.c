#include <kernel/arch.h>
#include <kernel/lib.h>

bool arch_signal_setup(struct regs *r, const struct siginfo *si, uint64_t mask, const struct linux_sigaction *sa) {
    (void)r, (void)si, (void)mask, (void)sa;
    panic("arm64: signal frames are not implemented yet");
}

bool arch_signal_return(struct regs *r, uint64_t *mask) {
    (void)r, (void)mask;
    panic("arm64: signal frames are not implemented yet");
}
