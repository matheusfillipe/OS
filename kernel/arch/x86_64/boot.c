#include <kernel/arch.h>
#include <kernel/boot.h>
#include <kernel/lib.h>
#include <kernel/proc.h>

static uint8_t bsp_df_stack[16384] __attribute__((aligned(16)));

void arch_init_bsp(struct cpu *c) {
    c->arch.df_stack = bsp_df_stack;
    gdt_init(c);
    cpu_set_percpu(c);
    idt_init();
    idt_load();
    kprintf("cpu: gdt, tss and idt loaded\n");
}

void arch_init_interrupts(struct cpu *c) {
    fpu_init_cpu();
    syscall_init_cpu();
    acpi_init();
    apic_init();
    c->arch.lapic_id = lapic_id();
}
