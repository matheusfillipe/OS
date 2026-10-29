#pragma once
#include <asm/cpu.h>
#include <asm/irq.h>
#include <asm/regs.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct thread;

/* Per-CPU state. `arch` must stay first: the arch reaches its per-CPU state through it. */
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

#define MAX_CPUS 256
extern struct cpu *cpus[MAX_CPUS];
extern uint32_t cpu_count;

/* Boot CPU setup, in kmain order: before memory management, then once the heap exists. */
void arch_init_bsp(struct cpu *c);
void arch_init_interrupts(struct cpu *c);
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

void arch_send_resched(struct cpu *c);

struct sbuf;
/* Starts the baseline the reported CPU frequency is measured against. */
void arch_cpuinfo_init(void);
/* Writes one /proc/cpuinfo block per CPU. */
void arch_cpuinfo(struct sbuf *b);
/* Base frequency in MHz, 0 when the CPU does not report it. */
uint32_t arch_base_mhz(void);

/* One cache as sysfs reports it. type is 1 data, 2 instruction, otherwise unified. */
struct arch_cache {
    uint32_t level, type, line, ways, parts, sets, sharing;
};
bool arch_cache_info(int index, struct arch_cache *ci);

void arch_mmu_init(void);
void arch_page_fault_init(void);
