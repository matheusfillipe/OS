#pragma once
#include <stdint.h>

/* Device registers through single loads and stores without writeback: a
 * hypervisor can emulate a trapped access only from the syndrome those report. */
static inline uint8_t mmio_read8(const volatile void *p) {
    uint8_t v;
    __asm__ volatile("ldrb %w0, [%1]" : "=r"(v) : "r"(p) : "memory");
    return v;
}
static inline uint16_t mmio_read16(const volatile void *p) {
    uint16_t v;
    __asm__ volatile("ldrh %w0, [%1]" : "=r"(v) : "r"(p) : "memory");
    return v;
}
static inline uint32_t mmio_read32(const volatile void *p) {
    uint32_t v;
    __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(p) : "memory");
    return v;
}
static inline uint64_t mmio_read64(const volatile void *p) {
    uint64_t v;
    __asm__ volatile("ldr %0, [%1]" : "=r"(v) : "r"(p) : "memory");
    return v;
}
static inline void mmio_write8(volatile void *p, uint8_t v) { __asm__ volatile("strb %w0, [%1]" ::"r"(v), "r"(p) : "memory"); }
static inline void mmio_write16(volatile void *p, uint16_t v) { __asm__ volatile("strh %w0, [%1]" ::"r"(v), "r"(p) : "memory"); }
static inline void mmio_write32(volatile void *p, uint32_t v) { __asm__ volatile("str %w0, [%1]" ::"r"(v), "r"(p) : "memory"); }
static inline void mmio_write64(volatile void *p, uint64_t v) { __asm__ volatile("str %0, [%1]" ::"r"(v), "r"(p) : "memory"); }
