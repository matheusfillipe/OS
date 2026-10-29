#pragma once
#include <stdint.h>

/* Device registers through single loads and stores without writeback: a
 * hypervisor can emulate a trapped access only from the syndrome those report. */
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
static inline void mmio_write32(volatile void *p, uint32_t v) { __asm__ volatile("str %w0, [%1]" ::"r"(v), "r"(p) : "memory"); }
static inline void mmio_write64(volatile void *p, uint64_t v) { __asm__ volatile("str %0, [%1]" ::"r"(v), "r"(p) : "memory"); }

/* arm64 has no I/O instructions: PCI I/O space is a memory window, which the
 * platform maps and points this at. */
extern volatile uint8_t *pci_io_base;

static inline void outb(uint16_t port, uint8_t v) { *(volatile uint8_t *)(pci_io_base + port) = v; }
static inline uint8_t inb(uint16_t port) { return *(volatile uint8_t *)(pci_io_base + port); }
static inline void outw(uint16_t port, uint16_t v) { *(volatile uint16_t *)(pci_io_base + port) = v; }
static inline uint16_t inw(uint16_t port) { return *(volatile uint16_t *)(pci_io_base + port); }
static inline void outl(uint16_t port, uint32_t v) { *(volatile uint32_t *)(pci_io_base + port) = v; }
static inline uint32_t inl(uint16_t port) { return *(volatile uint32_t *)(pci_io_base + port); }
