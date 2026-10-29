#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Register frame pushed by isr.S, in push order reversed. */
struct regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;
};

static inline uint64_t regs_ip(const struct regs *r) { return r->rip; }
static inline uint64_t regs_sp(const struct regs *r) { return r->rsp; }
static inline uint64_t regs_syscall_nr(const struct regs *r) { return r->rax; }
static inline bool regs_user_mode(const struct regs *r) { return (r->cs & 3) == 3; }
static inline void regs_set_return(struct regs *r, uint64_t v) { r->rax = v; }

static inline uint64_t regs_syscall_arg(const struct regs *r, int i) {
    switch (i) {
    case 0: return r->rdi;
    case 1: return r->rsi;
    case 2: return r->rdx;
    case 3: return r->r10;
    case 4: return r->r8;
    default: return r->r9;
    }
}
