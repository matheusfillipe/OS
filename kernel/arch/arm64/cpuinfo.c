#include <kernel/arch.h>
#include <kernel/lib.h>
#include <kernel/proc.h>
#include <kernel/sbuf.h>

#define read_sysreg(r)                                                                             \
    ({                                                                                             \
        uint64_t v_;                                                                               \
        __asm__ volatile("mrs %0, " #r : "=r"(v_));                                                \
        v_;                                                                                        \
    })

static uint64_t field(uint64_t reg, int shift) { return (reg >> shift) & 0xf; }

/* Linux AT_HWCAP bits, in bit order. */
static const char *const hwcap_names[] = {"fp", "asimd", "evtstrm", "aes", "pmull", "sha1", "sha2", "crc32", "atomics"};

uint64_t arch_hwcap(void) {
    uint64_t pfr0 = read_sysreg(id_aa64pfr0_el1), isar0 = read_sysreg(id_aa64isar0_el1), h = 0;
    if (field(pfr0, 16) != 0xf)
        h |= 1 << 0;
    if (field(pfr0, 20) != 0xf)
        h |= 1 << 1;
    if (field(isar0, 4) >= 1)
        h |= 1 << 3;
    if (field(isar0, 4) >= 2)
        h |= 1 << 4;
    if (field(isar0, 8) >= 1)
        h |= 1 << 5;
    if (field(isar0, 12) >= 1)
        h |= 1 << 6;
    if (field(isar0, 16) >= 1)
        h |= 1 << 7;
    if (field(isar0, 20) >= 2)
        h |= 1 << 8;
    return h;
}

void arch_cpuinfo_init(void) {}

void arch_cpuinfo(struct sbuf *b) {
    uint64_t midr = read_sysreg(midr_el1), hz = read_sysreg(cntfrq_el0), hwcap = arch_hwcap();
    for (uint32_t i = 0; i < cpu_count; i++) {
        sb_printf(b, "processor\t: %u\nBogoMIPS\t: %lu.%02lu\nFeatures\t:", i, hz / 500000, hz / 5000 % 100);
        for (size_t f = 0; f < ARRAY_LEN(hwcap_names); f++)
            if (hwcap & (1ULL << f))
                sb_printf(b, " %s", hwcap_names[f]);
        sb_printf(b, "\nCPU implementer\t: 0x%02lx\nCPU architecture: 8\nCPU variant\t: 0x%lx\n", midr >> 24 & 0xff,
                  midr >> 20 & 0xf);
        sb_printf(b, "CPU part\t: 0x%03lx\nCPU revision\t: %lu\n\n", midr >> 4 & 0xfff, midr & 0xf);
    }
}

uint32_t arch_base_mhz(void) { return 0; }

/* CLIDR_EL1 lists the cache type per level; a level with separate data and
 * instruction caches counts as two indices, data first, as Linux does. */
bool arch_cache_info(int index, struct arch_cache *ci) {
    uint64_t clidr = read_sysreg(clidr_el1);
    for (uint32_t level = 1; level <= 7; level++) {
        uint32_t ctype = (clidr >> (3 * (level - 1))) & 7;
        if (ctype == 0)
            return false;
        uint32_t types[2] = {ctype == 1 ? 2 : ctype == 4 ? 3 : 1, 2};
        for (int k = 0; k < (ctype == 3 ? 2 : 1); k++, index--) {
            if (index)
                continue;
            uint64_t f = irq_save();
            __asm__ volatile("msr csselr_el1, %0; isb" ::"r"((uint64_t)(level - 1) << 1 | (types[k] == 2)));
            uint64_t ccsidr = read_sysreg(ccsidr_el1);
            irq_restore(f);
            bool ccidx = field(read_sysreg(id_aa64mmfr2_el1), 20) == 1;
            ci->level = level;
            ci->type = types[k];
            ci->line = 1u << ((ccsidr & 7) + 4);
            ci->ways = (uint32_t)(ccidx ? (ccsidr >> 3 & 0x1fffff) : (ccsidr >> 3 & 0x3ff)) + 1;
            ci->sets = (uint32_t)(ccidx ? (ccsidr >> 32 & 0xffffff) : (ccsidr >> 13 & 0x7fff)) + 1;
            ci->parts = 1;
            /* The architecture does not say which CPUs share a cache: we take
             * the first level as private and the rest as shared by all. */
            ci->sharing = level == 1 ? 1 : cpu_count;
            return true;
        }
    }
    return false;
}
