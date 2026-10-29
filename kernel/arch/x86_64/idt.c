#include <asm/msr.h>
#include <kernel/arch.h>
#include <kernel/lib.h>
#include <linux/abi.h>

struct idt_entry {
    uint16_t offset_lo;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_hi;
    uint32_t zero;
} __attribute__((packed));

struct idtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

extern const uint64_t isr_table[256];
static struct idt_entry idt[256];
static irq_handler_t handlers[256];

static const char *const exception_names[32] = {
    "divide error", "debug", "NMI", "breakpoint", "overflow", "bound range",
    "invalid opcode", "device not available", "double fault", "coprocessor overrun",
    "invalid TSS", "segment not present", "stack fault", "general protection",
    "page fault", "reserved", "x87 FP", "alignment check", "machine check",
    "SIMD FP", "virtualization", "control protection", "reserved", "reserved",
    "reserved", "reserved", "reserved", "reserved", "hypervisor injection",
    "VMM communication", "security", "reserved",
};

static void set_gate(uint8_t vec, uint64_t addr, uint8_t ist, uint8_t dpl) {
    idt[vec] = (struct idt_entry){
        .offset_lo = addr & 0xffff,
        .selector = GDT_KERNEL_CODE,
        .ist = ist,
        .type_attr = 0x8e | (uint8_t)(dpl << 5), /* present, interrupt gate */
        .offset_mid = (addr >> 16) & 0xffff,
        .offset_hi = addr >> 32,
    };
}

void idt_init(void) {
    for (int i = 0; i < 256; i++)
        set_gate(i, isr_table[i], 0, 0);
    set_gate(8, isr_table[8], IST_DOUBLE_FAULT, 0);
    set_gate(3, isr_table[3], 0, 3); /* int3 allowed from user mode */
}

void idt_load(void) {
    struct idtr r = {sizeof idt - 1, (uint64_t)idt};
    __asm__ volatile("lidt %0" ::"m"(r));
}

void idt_set_handler(uint8_t vector, irq_handler_t handler) { handlers[vector] = handler; }

static void dump_regs(const struct regs *r) {
    kprintf("  rip=%016lx cs=%lx rflags=%016lx err=%lx\n", r->rip, r->cs, r->rflags, r->error);
    kprintf("  rsp=%016lx rbp=%016lx rax=%016lx\n", r->rsp, r->rbp, r->rax);
    kprintf("  rbx=%016lx rcx=%016lx rdx=%016lx\n", r->rbx, r->rcx, r->rdx);
    kprintf("  rsi=%016lx rdi=%016lx\n", r->rsi, r->rdi);
}

void signal_deliver(struct regs *r);
void user_exception(struct regs *r, uint64_t vector, int sig);
bool page_fault(struct regs *r, uint64_t addr, bool write, bool exec);

static void pf_handler(struct regs *r) {
    uint64_t addr = read_cr2();
    if (page_fault(r, addr, r->error & 2, r->error & 16))
        return;
    kprintf("\nkernel page fault at %lx, rip %lx, error %lx\n", addr, r->rip, r->error);
    panic("page fault in kernel mode");
}

void arch_page_fault_init(void) { idt_set_handler(14, pf_handler); }

static int exception_signal(uint64_t vector) {
    return vector == 0 || vector == 16 || vector == 19 ? SIGFPE
           : vector == 6                               ? SIGILL
           : vector == 3 || vector == 1                ? SIGTRAP
                                                       : SIGSEGV;
}

void isr_dispatch(struct regs *r);
void isr_dispatch(struct regs *r) {
    if (handlers[r->vector]) {
        handlers[r->vector](r);
        if (r->cs & 3)
            signal_deliver(r);
        return;
    }
    if (r->vector < 32 && (r->cs & 3)) {
        user_exception(r, r->vector, exception_signal(r->vector));
        return;
    }
    if (r->vector < 32) {
        kprintf("\nunhandled exception %lu (%s)\n", r->vector, exception_names[r->vector]);
        if (r->vector == 14)
            kprintf("  faulting address cr2=%016lx\n", read_cr2());
        dump_regs(r);
        panic("unhandled CPU exception");
    }
    if (r->vector != IRQ_SPURIOUS) {
        lapic_eoi();
        kprintf("unexpected interrupt vector %lu on cpu %u\n", r->vector, this_cpu()->id);
    }
}
