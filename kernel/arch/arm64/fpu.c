#include <kernel/arch.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/proc.h>

/* FP/SIMD state for user threads: V0-V31, FPSR and FPCR. The kernel itself
 * is built without FP/SIMD registers, so user state only needs saving when we
 * switch threads (eager save/restore, fpu_save and fpu_restore in entry.S).
 * The clean state is all zeros: round to nearest, no traps, no flags. */

#define CPACR_FPEN (3ULL << 20)

void fpu_init_cpu(void) {
    uint64_t cpacr;
    __asm__ volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
    __asm__ volatile("msr cpacr_el1, %0; isb" ::"r"(cpacr | CPACR_FPEN));
}

void *fpu_alloc(void) { return kzalloc(sizeof(struct fpsimd_context)); }

void fpu_free(void *area) { kfree(area); }

void fpu_copy(void *dst, const void *src) { memcpy(dst, src, sizeof(struct fpsimd_context)); }

void fpu_reset(void *area) { memset(area, 0, sizeof(struct fpsimd_context)); }

void fpu_capture_clean_state(void) { kprintf("fpu: fp/simd, state %lu bytes\n", sizeof(struct fpsimd_context)); }
