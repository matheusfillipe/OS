#include <kernel/lib.h>
#include <kernel/proc.h>

void fpu_init_cpu(void) { panic("arm64: FP/SIMD state is not implemented yet"); }

void *fpu_alloc(void) { panic("arm64: FP/SIMD state is not implemented yet"); }

void fpu_free(void *area) {
    (void)area;
    panic("arm64: FP/SIMD state is not implemented yet");
}

void fpu_save(void *area) {
    (void)area;
    panic("arm64: FP/SIMD state is not implemented yet");
}

void fpu_restore(void *area) {
    (void)area;
    panic("arm64: FP/SIMD state is not implemented yet");
}

void fpu_copy(void *dst, const void *src) {
    (void)dst, (void)src;
    panic("arm64: FP/SIMD state is not implemented yet");
}

void fpu_reset(void *area) {
    (void)area;
    panic("arm64: FP/SIMD state is not implemented yet");
}

void fpu_capture_clean_state(void) { panic("arm64: FP/SIMD state is not implemented yet"); }
