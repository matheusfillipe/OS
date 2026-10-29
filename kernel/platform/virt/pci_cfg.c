#include "virt.h"
#include <asm/io.h>
#include <kernel/boot.h>
#include <kernel/mm.h>
#include <kernel/platform.h>

/* PCI configuration access through ECAM: each function's 4 KiB of config
 * space is memory, mapped the first time we touch it. PCI I/O space is a
 * memory window too. */

static volatile uint32_t *cfg(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint64_t phys = VIRT_PCIE_ECAM | (uint64_t)bus << 20 | (uint64_t)dev << 15 | (uint64_t)fn << 12;
    vmm_map_mmio(phys, 0x1000, true);
    return phys_to_virt(phys + (off & 0xfc));
}

uint32_t platform_pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    return mmio_read32(cfg(bus, dev, fn, off));
}

void platform_pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v) {
    mmio_write32(cfg(bus, dev, fn, off), v);
}

static volatile void *pio(uint16_t port, unsigned size) {
    vmm_map_mmio(VIRT_PCIE_PIO + port, size, true);
    return phys_to_virt(VIRT_PCIE_PIO + port);
}

uint32_t platform_pci_io_read(uint16_t port, unsigned size) {
    volatile void *p = pio(port, size);
    return size == 1 ? mmio_read8(p) : size == 2 ? mmio_read16(p) : mmio_read32(p);
}

void platform_pci_io_write(uint16_t port, unsigned size, uint32_t v) {
    volatile void *p = pio(port, size);
    if (size == 1)
        mmio_write8(p, (uint8_t)v);
    else if (size == 2)
        mmio_write16(p, (uint16_t)v);
    else
        mmio_write32(p, v);
}
