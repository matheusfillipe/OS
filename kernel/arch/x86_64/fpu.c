#include <asm/msr.h>
#include <kernel/arch.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/proc.h>

/* FPU/SSE/AVX state for user threads. The kernel itself is built without
 * SSE, so user state only needs saving when we switch threads (eager
 * save/restore). XSAVE is used when available so AVX/AVX-512 registers
 * (which llama.cpp and whisper.cpp depend on) survive context switches. */

#define XSAVE_ALIGN 64
#define MSR_STAR  0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_FMASK 0xC0000084
#define EFER_SCE  1

static bool use_xsave;
static uint64_t xcr0;
static uint32_t area_size = 512;
static uint8_t *clean_state; /* state right after fninit, used for new threads */

static inline void xsetbv(uint32_t reg, uint64_t v) {
    __asm__ volatile("xsetbv" ::"c"(reg), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

void fpu_init_cpu(void) {
    uint32_t a, b, c, d;
    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2); /* EM: no emulation */
    cr0 &= ~(1ULL << 3); /* TS: no lazy trap */
    cr0 |= 1ULL << 1;    /* MP */
    cr0 |= 1ULL << 5;    /* NE: native FPU errors */
    __asm__ volatile("mov %0, %%cr0" ::"r"(cr0));

    cpuid(1, 0, &a, &b, &c, &d);
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9) | (1ULL << 10); /* OSFXSR, OSXMMEXCPT */
    bool has_xsave = c & (1u << 26);
    if (has_xsave)
        cr4 |= 1ULL << 18; /* OSXSAVE */
    __asm__ volatile("mov %0, %%cr4" ::"r"(cr4));

    if (has_xsave) {
        cpuid(0xd, 0, &a, &b, &c, &d);
        uint64_t supported = ((uint64_t)d << 32) | a;
        /* x87, SSE, AVX, and AVX-512 (opmask, ZMM_Hi256, Hi16_ZMM) if all present */
        uint64_t want = 0x7 & supported;
        if ((supported & 0xe0) == 0xe0)
            want |= 0xe0;
        xsetbv(0, want);
        cpuid(0xd, 0, &a, &b, &c, &d);
        if (!use_xsave) {
            use_xsave = true;
            xcr0 = want;
            area_size = b;
        }
    }
    __asm__ volatile("fninit");

    /* Enable the SYSCALL instruction for this CPU too. */
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
}

void *fpu_alloc(void) {
    uint8_t *raw = kzalloc(area_size + XSAVE_ALIGN + sizeof(void *));
    if (!raw)
        return NULL;
    uint8_t *area = (uint8_t *)ALIGN_UP((uint64_t)raw + sizeof(void *), XSAVE_ALIGN);
    ((void **)area)[-1] = raw;
    if (clean_state)
        memcpy(area, clean_state, area_size);
    return area;
}

void fpu_free(void *area) {
    if (area)
        kfree(((void **)area)[-1]);
}

void fpu_save(void *area) {
    if (use_xsave)
        __asm__ volatile("xsave64 (%0)" ::"r"(area), "a"((uint32_t)xcr0), "d"((uint32_t)(xcr0 >> 32)) : "memory");
    else
        __asm__ volatile("fxsave64 (%0)" ::"r"(area) : "memory");
}

void fpu_restore(void *area) {
    if (use_xsave)
        __asm__ volatile("xrstor64 (%0)" ::"r"(area), "a"((uint32_t)xcr0), "d"((uint32_t)(xcr0 >> 32)) : "memory");
    else
        __asm__ volatile("fxrstor64 (%0)" ::"r"(area) : "memory");
}

void fpu_copy(void *dst, const void *src) { memcpy(dst, src, area_size); }

void fpu_reset(void *area) { memcpy(area, clean_state, area_size); }

/* Called once on the BSP after the heap exists: capture a clean state with
 * the Linux default MXCSR (0x1f80, all exceptions masked). */
void fpu_capture_clean_state(void);
void fpu_capture_clean_state(void) {
    clean_state = NULL;
    uint8_t *area = fpu_alloc();
    KASSERT(area);
    __asm__ volatile("fninit");
    uint32_t mxcsr = 0x1f80;
    __asm__ volatile("ldmxcsr %0" ::"m"(mxcsr));
    fpu_save(area);
    clean_state = area;
    kprintf("fpu: %s, state %u bytes, xcr0=%lx\n", use_xsave ? "xsave" : "fxsave", area_size, xcr0);
}

void syscall_entry(void);

void syscall_init_cpu(void) {
    /* STAR: kernel CS at 0x08 (SS 0x10); user selectors derive from 0x10:
     * SS = 0x18|3, CS = 0x20|3, matching our GDT layout. */
    wrmsr(MSR_STAR, ((uint64_t)0x10 << 48) | ((uint64_t)GDT_KERNEL_CODE << 32));
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_FMASK, 0x40700); /* clear IF, TF, DF, AC on entry */
}
