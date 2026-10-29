#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/lib.h>
#include <kernel/mm.h>

#define DF_STACK_SIZE 16384

struct gdtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static uint64_t seg(uint8_t access, uint8_t flags) {
    return ((uint64_t)access << 40) | ((uint64_t)flags << 52);
}

/* Each CPU gets its own GDT because the TSS descriptor (and its busy bit)
 * is per CPU. */
void gdt_init(struct cpu *c) {
    uint64_t *gdt = c->arch.gdt;
    gdt[0] = 0;
    gdt[1] = seg(0x9a, 0x2); /* kernel code: present, ring0, exec/read, L=1 */
    gdt[2] = seg(0x92, 0x0); /* kernel data */
    gdt[3] = seg(0xf2, 0x0); /* user data, ring3 */
    gdt[4] = seg(0xfa, 0x2); /* user code, ring3, L=1 */

    if (!c->arch.df_stack) {
        c->arch.df_stack = kmalloc(DF_STACK_SIZE);
        KASSERT(c->arch.df_stack);
    }
    memset(&c->arch.tss, 0, sizeof c->arch.tss);
    c->arch.tss.ist[IST_DOUBLE_FAULT - 1] = (uint64_t)c->arch.df_stack + DF_STACK_SIZE;
    c->arch.tss.iomap_base = sizeof c->arch.tss;
    uint64_t base = (uint64_t)&c->arch.tss, limit = sizeof c->arch.tss - 1;
    gdt[5] = (limit & 0xffff) | ((base & 0xffffff) << 16) | (0x89ULL << 40) |
             (((limit >> 16) & 0xf) << 48) | (((base >> 24) & 0xff) << 56);
    gdt[6] = base >> 32;

    struct gdtr g = {sizeof c->arch.gdt - 1, (uint64_t)gdt};
    __asm__ volatile(
        "lgdt %0\n"
        "pushq %1\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "mov %2, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%ss\n"
        "xor %%eax, %%eax\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %3, %%ax\n"
        "ltr %%ax\n"
        :
        : "m"(g), "i"(GDT_KERNEL_CODE), "i"(GDT_KERNEL_DATA), "i"(GDT_TSS)
        : "rax", "memory");
}

#define MSR_GS_BASE 0xC0000101

/* Must run after gdt_init: loading %gs there clears the GS base. */
void cpu_set_percpu(struct cpu *c) {
    c->arch.self = c;
    wrmsr(MSR_GS_BASE, (uint64_t)c);
    wrmsr(0xC0000102, 0); /* KERNEL_GS_BASE: user GS while in kernel */
}

void cpu_enable_features(void) {
    uint32_t a, b, c, d;
    cpuid(0x80000001, 0, &a, &b, &c, &d);
    if (d & (1u << 20))
        wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE);
    __asm__ volatile("mov %%cr0, %%rax; or $0x10000, %%rax; mov %%rax, %%cr0" ::: "rax"); /* CR0.WP */
    /* PAT: make PA1 (selected by PTE.PWT alone) write-combining instead of
     * write-through, so framebuffer mappings are fast. Others stay default:
     * PA0 WB, PA2 UC-, PA3 UC, PA4 WB, PA5 WT, PA6 UC-, PA7 UC. */
    wrmsr(0x277, 0x0007040600070106ULL);
}
