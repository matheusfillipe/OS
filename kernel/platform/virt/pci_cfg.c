#include "virt.h"
#include <asm/io.h>
#include <kernel/boot.h>
#include <kernel/mm.h>
#include <kernel/platform.h>

/* PCI configuration access through ECAM: each function's 4 KiB of config
 * space is memory, mapped the first time we touch it. */

volatile uint8_t *pci_io_base;

static volatile uint32_t *cfg(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    /* Port I/O only reaches devices found through config space, so we map
     * its window on the first config access. */
    if (!pci_io_base) {
        vmm_map_mmio(VIRT_PCIE_PIO, VIRT_PCIE_PIO_SIZE, true);
        pci_io_base = phys_to_virt(VIRT_PCIE_PIO);
    }
    uint64_t phys = VIRT_PCIE_ECAM | (uint64_t)bus << 20 | (uint64_t)dev << 15 | (uint64_t)fn << 12;
    vmm_map_mmio(phys, 0x1000, true);
    return phys_to_virt(phys + (off & 0xfc));
}

uint32_t platform_pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) { return *cfg(bus, dev, fn, off); }

void platform_pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v) {
    *cfg(bus, dev, fn, off) = v;
}
