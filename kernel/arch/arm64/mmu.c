#include <kernel/arch.h>
#include <kernel/boot.h>
#include <kernel/lib.h>
#include <kernel/mm.h>

/* The kernel root stays in TTBR1. TTBR0 holds the user root, or an empty
 * table while no address space is loaded, so loading the kernel root means
 * loading that empty table. Without ASIDs every TTBR0 change flushes the
 * local TLB. */

#define TCR_T0SZ(n)   ((uint64_t)(n) << 0)
#define TCR_T1SZ(n)   ((uint64_t)(n) << 16)
#define TCR_WALK0     (1ULL << 8 | 1ULL << 10 | 3ULL << 12) /* write-back, inner shareable */
#define TCR_WALK1     (1ULL << 24 | 1ULL << 26 | 3ULL << 28)
#define TCR_TG1_4K    (2ULL << 30)
#define TCR_IPS(n)    ((uint64_t)(n) << 32)

static uint64_t empty_root[512] __attribute__((aligned(PAGE_SIZE)));

static uint64_t empty_root_phys(void) {
    struct limine_executable_address_response *ka = boot_kaddr_request.response;
    return (uint64_t)empty_root - ka->virtual_base + ka->physical_base;
}

/* We keep Limine's layout (4 KiB granules, 48-bit addresses in both halves),
 * so its tables stay valid until vmm_init replaces them. Output addresses
 * above 48 bits would need the LPA2 descriptor format. */
void arch_mmu_init(void) {
    uint64_t mmfr0;
    __asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
    uint64_t ips = mmfr0 & 0xf;
    if (ips > 5)
        ips = 5;
    uint64_t tcr = TCR_T0SZ(16) | TCR_T1SZ(16) | TCR_WALK0 | TCR_WALK1 | TCR_TG1_4K | TCR_IPS(ips);
    __asm__ volatile("msr mair_el1, %0; msr tcr_el1, %1; isb; tlbi vmalle1; dsb nsh; isb" ::"r"(MAIR_VALUE), "r"(tcr)
                     : "memory");
}

void arch_load_root(uint64_t phys) {
    if (phys == virt_to_phys_hhdm(vmm_kernel_pgd())) {
        __asm__ volatile("dsb ishst; msr ttbr1_el1, %0" ::"r"(phys) : "memory");
        phys = empty_root_phys();
    }
    __asm__ volatile("dsb ishst; msr ttbr0_el1, %0; isb; tlbi vmalle1; dsb nsh; isb" ::"r"(phys) : "memory");
}

uint64_t arch_current_root(void) {
    uint64_t ttbr0;
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(ttbr0));
    ttbr0 &= PTE_ADDR;
    return ttbr0 == empty_root_phys() ? virt_to_phys_hhdm(vmm_kernel_pgd()) : ttbr0;
}
