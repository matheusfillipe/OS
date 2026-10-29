#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Register frame saved on exception entry. */
struct regs {
    uint64_t x[31];
    uint64_t sp, pc, pstate;
    uint64_t esr, far;
};

/* A thread's FP/SIMD registers, kept in the layout of the Linux signal
 * frame's fpsimd_context record. */
struct fpsimd_context {
    uint32_t magic, size;
    uint32_t fpsr, fpcr;
    uint64_t vregs[64];
};

static inline uint64_t regs_ip(const struct regs *r) { return r->pc; }
static inline void regs_set_ip(struct regs *r, uint64_t ip) { r->pc = ip; }
static inline uint64_t regs_sp(const struct regs *r) { return r->sp; }
static inline uint64_t regs_syscall_nr(const struct regs *r) { return r->x[8]; }
static inline bool regs_user_mode(const struct regs *r) { return (r->pstate & 0xf) == 0; }
static inline uint64_t regs_return(const struct regs *r) { return r->x[0]; }
static inline void regs_set_return(struct regs *r, uint64_t v) { r->x[0] = v; }
static inline uint64_t regs_syscall_arg(const struct regs *r, int i) { return r->x[i]; }
