#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <kernel/mm.h>

/* arm64 VMSAv8-64 descriptors, 4 KiB granule, 48-bit virtual addresses. */
#define PTE_VALID  (1ULL << 0)
#define PTE_TABLE  (1ULL << 1) /* table at levels 0-2, page at level 3; clear: block */
#define PTE_ATTR(i) ((uint64_t)(i) << 2)
#define PTE_ATTR_MASK PTE_ATTR(7)
#define PTE_EL0    (1ULL << 6)
#define PTE_RDONLY (1ULL << 7)
#define PTE_ISH    (3ULL << 8)
#define PTE_AF     (1ULL << 10)
#define PTE_NG     (1ULL << 11)
#define PTE_PXN    (1ULL << 53)
#define PTE_UXN    (1ULL << 54)
#define PTE_COW    (1ULL << 55) /* software bit */
#define PTE_ADDR   0x0000fffffffff000ULL

/* MAIR_EL1 attribute indices. Index 0 is Limine's normal write-back too, so
 * its mappings keep their meaning when we load our MAIR. */
#define MAIR_NORMAL 0
#define MAIR_NC     1
#define MAIR_DEVICE 2
#define MAIR_DEVICE_ATTR 0x04 /* Device-nGnRE */
#define MAIR_VALUE  (0xffULL | 0x44ULL << 8 | (uint64_t)MAIR_DEVICE_ATTR << 16)

/* The kernel lives in TTBR1 and user space in TTBR0, so a user root shares
 * nothing with the kernel's. */
#define USER_PGD_ENTRIES 512

#define KERNEL_VIRT_BASE 0xffff000000000000ULL

static inline uint64_t pte_bits(uint64_t pg) {
    uint64_t e = PTE_VALID | PTE_AF | PTE_ISH;
    if (!(pg & PG_WRITE))
        e |= PTE_RDONLY;
    if (pg & PG_USER)
        e |= PTE_EL0;
    if (!(pg & PG_GLOBAL))
        e |= PTE_NG;
    if (pg & PG_UNCACHED)
        e |= PTE_ATTR(MAIR_DEVICE);
    else if (pg & PG_WC)
        e |= PTE_ATTR(MAIR_NC);
    else
        e |= PTE_ATTR(MAIR_NORMAL);
    if (pg & PG_COW)
        e |= PTE_COW;
    if (!(pg & PG_EXEC))
        e |= PTE_UXN | PTE_PXN;
    else
        e |= (pg & PG_USER) ? PTE_PXN : PTE_UXN;
    return e;
}

static inline uint64_t pte_make(uint64_t phys, uint64_t pg) { return (phys & PTE_ADDR) | pte_bits(pg) | PTE_TABLE; }
static inline uint64_t pte_make_huge(uint64_t phys, uint64_t pg) { return (phys & PTE_ADDR) | pte_bits(pg); }
static inline uint64_t pte_table(uint64_t phys, bool user) {
    (void)user;
    return (phys & PTE_ADDR) | PTE_VALID | PTE_TABLE;
}

static inline bool pte_present(uint64_t e) { return e & PTE_VALID; }
static inline bool pte_is_huge(uint64_t e) { return (e & (PTE_VALID | PTE_TABLE)) == PTE_VALID; }
static inline uint64_t pte_phys(uint64_t e) { return e & PTE_ADDR; }

static inline uint64_t pte_flags(uint64_t e) {
    uint64_t pg = 0;
    if (!(e & PTE_RDONLY))
        pg |= PG_WRITE;
    if (e & PTE_EL0)
        pg |= PG_USER;
    if (!(e & ((e & PTE_EL0) ? PTE_UXN : PTE_PXN)))
        pg |= PG_EXEC;
    if (!(e & PTE_NG))
        pg |= PG_GLOBAL;
    if ((e & PTE_ATTR_MASK) == PTE_ATTR(MAIR_DEVICE))
        pg |= PG_UNCACHED;
    else if ((e & PTE_ATTR_MASK) == PTE_ATTR(MAIR_NC))
        pg |= PG_WC;
    if (e & PTE_COW)
        pg |= PG_COW;
    return pg;
}

static inline void arch_tlb_flush_page(uint64_t va) {
    __asm__ volatile("dsb ishst; tlbi vaae1is, %0; dsb ish; isb" ::"r"((va >> 12) & 0xfffffffffffULL) : "memory");
}
static inline void arch_tlb_flush_all(void) { __asm__ volatile("dsb ishst; tlbi vmalle1is; dsb ish; isb" ::: "memory"); }
void arch_load_root(uint64_t phys);
uint64_t arch_current_root(void);
