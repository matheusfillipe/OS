#pragma once
#include <asm/cpu.h>
#include <asm/irq.h>
#include <asm/regs.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct thread;

/* Per-CPU state, reached through the GS base register. `arch` must stay
 * first: this_cpu() reads its `self` with a single gs-relative load. */
struct cpu {
    struct arch_cpu arch;
    uint32_t id;       /* 0..cpu_count-1, BSP is 0 */
    struct thread *current;
    struct thread *idle;
    struct thread *prev; /* thread we just switched away from */
    uint64_t ticks;
    volatile bool online;
    struct mm *active_mm;          /* user page tables loaded on this CPU, or NULL */
    volatile int tlb_flush_pending; /* set by tlb_shootdown, cleared by the flush */
};

_Static_assert(offsetof(struct cpu, arch.self) == 0, "this_cpu() reads self at gs:0");
_Static_assert(offsetof(struct cpu, arch.kernel_rsp) == 8, "syscall.S reads kernel_rsp at gs:8");
_Static_assert(offsetof(struct cpu, arch.user_rsp) == 16, "syscall.S uses user_rsp at gs:16");

#define MAX_CPUS 256
extern struct cpu *cpus[MAX_CPUS];
extern uint32_t cpu_count;

void arch_selftest_breakpoint(void);

void switch_context(uint64_t *save_sp, uint64_t new_sp);
uint64_t arch_thread_initial_sp(uint64_t stack_top);
void arch_set_kernel_stack(struct cpu *c, uint64_t top);
void arch_load_tls(uint64_t tls);
void arch_exec_init_regs(struct regs *r, uint64_t entry, uint64_t sp);
void arch_fork_child_regs(struct regs *r, uint64_t new_sp);

struct siginfo;
struct linux_sigaction;
bool arch_signal_setup(struct regs *r, const struct siginfo *si, uint64_t mask, const struct linux_sigaction *sa);
bool arch_signal_return(struct regs *r, uint64_t *mask);

uint64_t arch_hwcap(void);

void arch_mmu_init(void);
void arch_page_fault_init(void);
