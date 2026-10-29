#pragma once
#include <stdint.h>

/* arm64 has no I/O instructions: PCI I/O space is a memory window, which the
 * platform maps and points this at. */
extern volatile uint8_t *pci_io_base;

static inline void outb(uint16_t port, uint8_t v) { *(volatile uint8_t *)(pci_io_base + port) = v; }
static inline uint8_t inb(uint16_t port) { return *(volatile uint8_t *)(pci_io_base + port); }
static inline void outw(uint16_t port, uint16_t v) { *(volatile uint16_t *)(pci_io_base + port) = v; }
static inline uint16_t inw(uint16_t port) { return *(volatile uint16_t *)(pci_io_base + port); }
static inline void outl(uint16_t port, uint32_t v) { *(volatile uint32_t *)(pci_io_base + port) = v; }
static inline uint32_t inl(uint16_t port) { return *(volatile uint32_t *)(pci_io_base + port); }
