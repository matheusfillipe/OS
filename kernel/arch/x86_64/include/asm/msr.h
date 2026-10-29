#pragma once
#include <stdint.h>

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile("wrmsr" ::"c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}
static inline void cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}
static inline uint64_t read_cr2(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}
static inline uint64_t read_cr3(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}
static inline void write_cr3(uint64_t v) { __asm__ volatile("mov %0, %%cr3" ::"r"(v) : "memory"); }
static inline void invlpg(uint64_t va) { __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory"); }

#define MSR_EFER 0xC0000080
#define EFER_NXE (1ULL << 11)
#define MSR_FS_BASE 0xC0000100
