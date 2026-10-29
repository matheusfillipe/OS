#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <asm/msr.h>
#include <kernel/mm.h>

/* x86-64 long mode page table entry bits. */
#define PTE_PRESENT (1ULL << 0)
#define PTE_WRITE   (1ULL << 1)
#define PTE_USER    (1ULL << 2)
#define PTE_PWT     (1ULL << 3)
#define PTE_PCD     (1ULL << 4)
#define PTE_HUGE    (1ULL << 7)
#define PTE_GLOBAL  (1ULL << 8)
#define PTE_COW     (1ULL << 9) /* software bit */
#define PTE_NX      (1ULL << 63)
#define PTE_ADDR    0x000ffffffffff000ULL

/* User space owns the lower half of the root; every address space shares the
 * kernel's upper-half entries. */
#define USER_PGD_ENTRIES 256

extern bool have_nx;

static inline uint64_t pte_bits(uint64_t pg) {
    uint64_t e = PTE_PRESENT;
    if (pg & PG_WRITE)
        e |= PTE_WRITE;
    if (pg & PG_USER)
        e |= PTE_USER;
    if (pg & PG_GLOBAL)
        e |= PTE_GLOBAL;
    if (pg & PG_UNCACHED)
        e |= PTE_PCD | PTE_PWT;
    if (pg & PG_WC)
        e |= PTE_PWT; /* PAT entry 1, selected by PWT alone, is write-combining (gdt.c) */
    if (pg & PG_COW)
        e |= PTE_COW;
    if (!(pg & PG_EXEC) && have_nx)
        e |= PTE_NX;
    return e;
}

static inline uint64_t pte_make(uint64_t phys, uint64_t pg) { return (phys & PTE_ADDR) | pte_bits(pg); }
static inline uint64_t pte_make_huge(uint64_t phys, uint64_t pg) {
    return (phys & PTE_ADDR) | pte_bits(pg) | PTE_HUGE;
}
static inline uint64_t pte_table(uint64_t phys, bool user) {
    return (phys & PTE_ADDR) | PTE_PRESENT | PTE_WRITE | (user ? PTE_USER : 0);
}

static inline bool pte_present(uint64_t e) { return e & PTE_PRESENT; }
static inline bool pte_is_huge(uint64_t e) { return e & PTE_HUGE; }
static inline uint64_t pte_phys(uint64_t e) { return e & PTE_ADDR; }

static inline uint64_t pte_flags(uint64_t e) {
    uint64_t pg = 0;
    if (e & PTE_WRITE)
        pg |= PG_WRITE;
    if (e & PTE_USER)
        pg |= PG_USER;
    if (!(e & PTE_NX))
        pg |= PG_EXEC;
    if (e & PTE_GLOBAL)
        pg |= PG_GLOBAL;
    if (e & PTE_PCD)
        pg |= PG_UNCACHED;
    else if (e & PTE_PWT)
        pg |= PG_WC;
    if (e & PTE_COW)
        pg |= PG_COW;
    return pg;
}

static inline void arch_tlb_flush_page(uint64_t va) { invlpg(va); }
static inline void arch_tlb_flush_all(void) { write_cr3(read_cr3()); }
static inline void arch_load_root(uint64_t phys) { write_cr3(phys); }
static inline uint64_t arch_current_root(void) { return read_cr3(); }
