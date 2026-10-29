#include <asm/msr.h>
#include <kernel/arch.h>
#include <kernel/lib.h>
#include <kernel/proc.h>
#include <kernel/sbuf.h>

struct flag_name {
    uint8_t bit;
    const char *name;
};

static const struct flag_name f1_edx[] = {
    {0, "fpu"}, {1, "vme"}, {2, "de"}, {3, "pse"}, {4, "tsc"}, {5, "msr"}, {6, "pae"}, {7, "mce"},
    {8, "cx8"}, {9, "apic"}, {11, "sep"}, {12, "mtrr"}, {13, "pge"}, {14, "mca"}, {15, "cmov"},
    {16, "pat"}, {17, "pse36"}, {19, "clflush"}, {21, "dts"}, {22, "acpi"}, {23, "mmx"}, {24, "fxsr"},
    {25, "sse"}, {26, "sse2"}, {27, "ss"}, {28, "ht"}, {29, "tm"}, {31, "pbe"}, {0, NULL},
};
static const struct flag_name e1_edx[] = {
    {11, "syscall"}, {20, "nx"}, {22, "mmxext"}, {25, "fxsr_opt"}, {26, "pdpe1gb"}, {27, "rdtscp"},
    {29, "lm"}, {0, NULL},
};
static const struct flag_name f1_ecx[] = {
    {0, "pni"}, {1, "pclmulqdq"}, {2, "dtes64"}, {3, "monitor"}, {4, "ds_cpl"}, {5, "vmx"}, {6, "smx"},
    {7, "est"}, {8, "tm2"}, {9, "ssse3"}, {11, "sdbg"}, {12, "fma"}, {13, "cx16"}, {14, "xtpr"},
    {15, "pdcm"}, {17, "pcid"}, {18, "dca"}, {19, "sse4_1"}, {20, "sse4_2"}, {21, "x2apic"},
    {22, "movbe"}, {23, "popcnt"}, {24, "tsc_deadline_timer"}, {25, "aes"}, {26, "xsave"}, {28, "avx"},
    {29, "f16c"}, {30, "rdrand"}, {31, "hypervisor"}, {0, NULL},
};
static const struct flag_name e1_ecx[] = {
    {0, "lahf_lm"}, {1, "cmp_legacy"}, {2, "svm"}, {3, "extapic"}, {4, "cr8_legacy"}, {5, "abm"},
    {6, "sse4a"}, {7, "misalignsse"}, {8, "3dnowprefetch"}, {9, "osvw"}, {10, "ibs"}, {11, "xop"},
    {12, "skinit"}, {13, "wdt"}, {15, "lwp"}, {16, "fma4"}, {17, "tce"}, {19, "nodeid_msr"}, {21, "tbm"},
    {22, "topoext"}, {23, "perfctr_core"}, {24, "perfctr_nb"}, {0, NULL},
};
static const struct flag_name f7_ebx[] = {
    {0, "fsgsbase"}, {1, "tsc_adjust"}, {3, "bmi1"}, {4, "hle"}, {5, "avx2"}, {7, "smep"}, {8, "bmi2"},
    {9, "erms"}, {10, "invpcid"}, {11, "rtm"}, {16, "avx512f"}, {17, "avx512dq"}, {18, "rdseed"},
    {19, "adx"}, {20, "smap"}, {21, "avx512ifma"}, {23, "clflushopt"}, {24, "clwb"}, {26, "avx512pf"},
    {27, "avx512er"}, {28, "avx512cd"}, {29, "sha_ni"}, {30, "avx512bw"}, {31, "avx512vl"}, {0, NULL},
};
static const struct flag_name f7_ecx[] = {
    {1, "avx512vbmi"}, {2, "umip"}, {3, "pku"}, {4, "ospke"}, {5, "waitpkg"}, {6, "avx512_vbmi2"},
    {8, "gfni"}, {9, "vaes"}, {10, "vpclmulqdq"}, {11, "avx512_vnni"}, {12, "avx512_bitalg"},
    {14, "avx512_vpopcntdq"}, {16, "la57"}, {22, "rdpid"}, {25, "cldemote"}, {27, "movdiri"},
    {28, "movdir64b"}, {0, NULL},
};
static const struct flag_name f7_edx[] = {
    {2, "avx512_4vnniw"}, {3, "avx512_4fmaps"}, {4, "fsrm"}, {8, "avx512_vp2intersect"}, {10, "md_clear"},
    {14, "serialize"}, {16, "tsxldtrk"}, {18, "pconfig"}, {22, "amx_bf16"}, {23, "avx512_fp16"},
    {24, "amx_tile"}, {25, "amx_int8"}, {0, NULL},
};
static const struct flag_name f71_eax[] = {{4, "avx_vnni"}, {5, "avx512_bf16"}, {0, NULL}};
static const struct flag_name fd1_eax[] = {{0, "xsaveopt"}, {1, "xsavec"}, {2, "xgetbv1"}, {3, "xsaves"}, {0, NULL}};

static void put_flags(struct sbuf *b, const struct flag_name *t, uint32_t reg) {
    for (; t->name; t++)
        if (reg & (1u << t->bit))
            sb_printf(b, " %s", t->name);
}

static uint64_t counter0, ns0;

void arch_cpuinfo_init(void) {
    counter0 = arch_counter();
    ns0 = time_monotonic_ns();
}

static uint64_t counter_khz(void) {
    uint64_t ns = time_monotonic_ns() - ns0;
    uint64_t ticks = arch_counter() - counter0;
    return ns > 1000000 ? ticks / (ns / 1000000) : 0;
}

void arch_cpuinfo(struct sbuf *b) {
    uint64_t khz = counter_khz();
    uint32_t a, bx, c, d, max, maxext;
    char vendor[13] = {0}, model[49] = {0};
    cpuid(0, 0, &max, &bx, &c, &d);
    memcpy(vendor, &bx, 4), memcpy(vendor + 4, &d, 4), memcpy(vendor + 8, &c, 4);
    cpuid(0x80000000, 0, &maxext, &bx, &c, &d);
    if (maxext >= 0x80000004) {
        for (uint32_t i = 0; i < 3; i++) {
            uint32_t r[4];
            cpuid(0x80000002 + i, 0, &r[0], &r[1], &r[2], &r[3]);
            memcpy(model + i * 16, r, 16);
        }
    }
    const char *mname = model;
    while (*mname == ' ')
        mname++;
    for (size_t l = strlen(model); l && model[l - 1] == ' '; l--)
        model[l - 1] = 0;
    uint32_t sig, misc, ecx1, edx1;
    cpuid(1, 0, &sig, &misc, &ecx1, &edx1);
    uint32_t family = (sig >> 8) & 0xf, mdl = (sig >> 4) & 0xf, stepping = sig & 0xf;
    if (family == 0xf)
        family += (sig >> 20) & 0xff;
    if (family == 6 || family >= 0xf)
        mdl += ((sig >> 16) & 0xf) << 4;
    uint32_t b7 = 0, c7 = 0, d7 = 0, a71 = 0, ad1 = 0, ce = 0, de = 0, phys_bits = 40, virt_bits = 48, l2kb = 0;
    if (max >= 7) {
        cpuid(7, 0, &a, &b7, &c7, &d7);
        if (a >= 1)
            cpuid(7, 1, &a71, &bx, &c, &d);
    }
    if (max >= 0xd)
        cpuid(0xd, 1, &ad1, &bx, &c, &d);
    if (maxext >= 0x80000001)
        cpuid(0x80000001, 0, &a, &bx, &ce, &de);
    if (maxext >= 0x80000006) {
        cpuid(0x80000006, 0, &a, &bx, &c, &d);
        l2kb = c >> 16;
    }
    if (maxext >= 0x80000008) {
        cpuid(0x80000008, 0, &a, &bx, &c, &d);
        phys_bits = a & 0xff;
        virt_bits = (a >> 8) & 0xff;
    }
    for (uint32_t i = 0; i < cpu_count; i++) {
        sb_printf(b, "processor\t: %u\nvendor_id\t: %s\ncpu family\t: %u\nmodel\t\t: %u\nmodel name\t: %s\n", i,
                  vendor, family, mdl, *mname ? mname : "unknown");
        sb_printf(b, "stepping\t: %u\nmicrocode\t: 0x1\ncpu MHz\t\t: %lu.%03lu\ncache size\t: %u KB\n", stepping,
                  khz / 1000, khz % 1000, l2kb);
        sb_printf(b, "physical id\t: 0\nsiblings\t: %u\ncore id\t\t: %u\ncpu cores\t: %u\n", cpu_count, i,
                  cpu_count);
        sb_printf(b, "apicid\t\t: %u\ninitial apicid\t: %u\nfpu\t\t: yes\nfpu_exception\t: yes\n",
                  cpus[i] ? cpus[i]->arch.lapic_id : i, cpus[i] ? cpus[i]->arch.lapic_id : i);
        sb_printf(b, "cpuid level\t: %u\nwp\t\t: yes\nflags\t\t:", max);
        put_flags(b, f1_edx, edx1);
        put_flags(b, e1_edx, de);
        put_flags(b, f1_ecx, ecx1);
        put_flags(b, e1_ecx, ce);
        put_flags(b, f7_ebx, b7);
        put_flags(b, f7_ecx, c7);
        put_flags(b, f7_edx, d7);
        put_flags(b, f71_eax, a71);
        put_flags(b, fd1_eax, ad1);
        sb_printf(b, "\nbugs\t\t:\nbogomips\t: %lu.%02lu\nclflush size\t: %u\ncache_alignment\t: 64\n",
                  khz * 2 / 1000, (khz * 2 / 10) % 100, ((misc >> 8) & 0xff) * 8);
        sb_printf(b, "address sizes\t: %u bits physical, %u bits virtual\npower management:\n\n", phys_bits,
                  virt_bits);
    }
}


uint32_t arch_base_mhz(void) {
    /* CPUID leaf 0x16 has the base frequency in MHz where it is reported. */
    uint32_t a, bx, c, d, max;
    cpuid(0, 0, &max, &bx, &c, &d);
    if (max < 0x16)
        return 0;
    cpuid(0x16, 0, &a, &bx, &c, &d);
    return a & 0xffff;
}

/* CPUID leaf 4 on Intel, 0x8000001D on AMD; same layout. */
bool arch_cache_info(int index, struct arch_cache *ci) {
    uint32_t r[4], max, extmax, bx, c, d;
    bool found = false;
    cpuid(0, 0, &max, &bx, &c, &d);
    if (max >= 4) {
        cpuid(4, (uint32_t)index, &r[0], &r[1], &r[2], &r[3]);
        if (r[0] & 0x1f)
            found = true;
        else if (index > 0)
            return false;
    }
    if (!found) {
        cpuid(0x80000000, 0, &extmax, &bx, &c, &d);
        if (extmax < 0x8000001d)
            return false;
        cpuid(0x8000001d, (uint32_t)index, &r[0], &r[1], &r[2], &r[3]);
        if (!(r[0] & 0x1f))
            return false;
    }
    ci->level = (r[0] >> 5) & 7;
    ci->type = r[0] & 0x1f;
    ci->sharing = ((r[0] >> 14) & 0xfff) + 1;
    ci->ways = (r[1] >> 22) + 1;
    ci->parts = ((r[1] >> 12) & 0x3ff) + 1;
    ci->line = (r[1] & 0xfff) + 1;
    ci->sets = r[2] + 1;
    return true;
}
