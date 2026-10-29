#include <kernel/arch.h>
#include <kernel/lib.h>

static volatile int breakpoint_hits;

static void on_breakpoint(struct regs *r) {
    (void)r;
    breakpoint_hits++;
}

void arch_selftest_breakpoint(void) {
    idt_set_handler(3, on_breakpoint);
    __asm__ volatile("int3");
    KASSERT(breakpoint_hits == 1);
    idt_set_handler(3, NULL);
    kprintf("selftest: interrupts ok\n");
}
