#include <kernel/arch.h>
#include <kernel/boot.h>
#include <kernel/spinlock.h>
#include <kernel/lib.h>
#include <kernel/mm.h>

#define HUGE_PAGE (2UL * 1024 * 1024)

extern char __kernel_start[], __text_start[], __text_end[], __rodata_start[], __rodata_end[],
    __data_start[], __kernel_end[];

static uint64_t *kernel_pgd;

static uint64_t *alloc_table(void) {
    uint64_t phys = pmm_alloc(1);
    if (!phys)
        return NULL;
    uint64_t *t = phys_to_virt(phys);
    memset(t, 0, PAGE_SIZE);
    return t;
}

/* Returns the table that `level` (3 = PDPT, 2 = PD, 1 = PT) entries for
 * `virt` live in, creating missing intermediate tables. */
static uint64_t *walk(uint64_t *pgd, uint64_t virt, int level, uint64_t flags) {
    uint64_t *table = pgd;
    for (int l = 4; l > level; l--) {
        uint64_t idx = (virt >> (12 + 9 * (l - 1))) & 0x1ff;
        uint64_t e = table[idx];
        if (!pte_present(e)) {
            uint64_t *next = alloc_table();
            if (!next)
                return NULL;
            e = pte_table(virt_to_phys_hhdm(next), flags & PG_USER);
            table[idx] = e;
        } else if (pte_is_huge(e)) {
            return NULL; /* splitting huge pages is not supported yet */
        } else if (flags & PG_USER) {
            table[idx] = pte_table(pte_phys(e), true);
        }
        table = phys_to_virt(pte_phys(e));
    }
    return table;
}

bool vmm_map(uint64_t *pgd, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *pt = walk(pgd, virt, 1, flags);
    if (!pt)
        return false;
    pt[(virt >> 12) & 0x1ff] = pte_make(phys, flags);
    return true;
}

bool vmm_map_huge(uint64_t *pgd, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *pd = walk(pgd, virt, 2, flags);
    if (!pd)
        return false;
    pd[(virt >> 21) & 0x1ff] = pte_make_huge(phys & ~(HUGE_PAGE - 1), flags);
    return true;
}

uint64_t vmm_translate(uint64_t *pgd, uint64_t virt) {
    uint64_t *table = pgd;
    for (int l = 4; l >= 1; l--) {
        uint64_t e = table[(virt >> (12 + 9 * (l - 1))) & 0x1ff];
        if (!pte_present(e))
            return 0;
        if (l == 2 && pte_is_huge(e))
            return (pte_phys(e) & ~(HUGE_PAGE - 1)) + (virt & (HUGE_PAGE - 1));
        if (l == 1)
            return pte_phys(e) + (virt & 0xfff);
        table = phys_to_virt(pte_phys(e));
    }
    return 0;
}

uint64_t *vmm_kernel_pgd(void) { return kernel_pgd; }

static spinlock_t kernel_map_lock = SPINLOCK_INIT;

void vmm_map_mmio(uint64_t phys, uint64_t len, bool uncached) {
    uint64_t flags = PG_WRITE | PG_GLOBAL | (uncached ? PG_UNCACHED : 0);
    uint64_t f = spin_lock_irqsave(&kernel_map_lock);
    for (uint64_t p = ALIGN_DOWN(phys, PAGE_SIZE); p < phys + len; p += PAGE_SIZE)
        if (!vmm_translate(kernel_pgd, p + hhdm_offset) && !vmm_map(kernel_pgd, p + hhdm_offset, p, flags))
            panic("vmm: cannot map MMIO page %lx", p);
    spin_unlock_irqrestore(&kernel_map_lock, f);
}

static void map_kernel_range(uint64_t phys_base, uint64_t virt_base, char *start, char *end, uint64_t flags) {
    for (uint64_t v = ALIGN_DOWN((uint64_t)start, PAGE_SIZE); v < (uint64_t)end; v += PAGE_SIZE)
        if (!vmm_map(kernel_pgd, v, v - virt_base + phys_base, flags | PG_GLOBAL))
            panic("vmm: cannot map kernel page %lx", v);
}

void vmm_init(void) {
    struct limine_executable_address_response *ka = boot_kaddr_request.response;
    struct limine_memmap_response *mm = boot_memmap_request.response;
    KASSERT(ka && mm);

    arch_mmu_init();

    kernel_pgd = alloc_table();
    KASSERT(kernel_pgd);

    /* Kernel image, W^X: code is read-only + executable, rodata read-only,
     * everything else read/write but never executable. */
    uint64_t pb = ka->physical_base, vb = ka->virtual_base;
    map_kernel_range(pb, vb, __kernel_start, __text_start, PG_WRITE);
    map_kernel_range(pb, vb, __text_start, __text_end, PG_EXEC);
    map_kernel_range(pb, vb, __rodata_start, __rodata_end, 0);
    map_kernel_range(pb, vb, __data_start, __kernel_end, PG_WRITE);

    /* Direct map of all physical memory the bootloader told us about. */
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type == LIMINE_MEMMAP_BAD_MEMORY)
            continue;
        uint64_t flags = PG_WRITE;
        if (e->type == LIMINE_MEMMAP_FRAMEBUFFER)
            flags |= PG_WC;
        for (uint64_t p = ALIGN_DOWN(e->base, HUGE_PAGE); p < e->base + e->length; p += HUGE_PAGE)
            if (!vmm_map_huge(kernel_pgd, p + hhdm_offset, p, flags))
                panic("vmm: cannot direct-map %lx", p);
    }

    /* Pre-create every upper-half top-level slot so user address spaces,
     * which copy these entries once, see all future kernel mappings. */
    for (int i = USER_PGD_ENTRIES; i < 512; i++) {
        if (!pte_present(kernel_pgd[i])) {
            uint64_t *t = alloc_table();
            KASSERT(t);
            kernel_pgd[i] = pte_table(virt_to_phys_hhdm(t), false);
        }
    }

    arch_load_root(virt_to_phys_hhdm(kernel_pgd));
}
