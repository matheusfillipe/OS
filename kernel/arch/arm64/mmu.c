#include <kernel/arch.h>
#include <kernel/lib.h>
#include <kernel/mm.h>

void arch_mmu_init(void) { panic("arm64: MMU setup (MAIR, TCR) is not implemented yet"); }

void arch_page_fault_init(void) { panic("arm64: page fault handling is not implemented yet"); }

void arch_load_root(uint64_t phys) {
    (void)phys;
    panic("arm64: switching between TTBR0 and TTBR1 roots is not implemented yet");
}

uint64_t arch_current_root(void) { panic("arm64: switching between TTBR0 and TTBR1 roots is not implemented yet"); }
