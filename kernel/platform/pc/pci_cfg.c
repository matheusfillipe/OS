#include <asm/io.h>
#include <kernel/platform.h>

/* PCI configuration access through the legacy 0xCF8/0xCFC ports, which every
 * x86 PC still provides. ECAM (MCFG) access replaces it for PCIe extended
 * config space when a driver needs it. */

static uint32_t cfg_addr(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    return 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | (off & 0xfc);
}

uint32_t platform_pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    outl(0xcf8, cfg_addr(bus, dev, fn, off));
    return inl(0xcfc);
}

void platform_pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v) {
    outl(0xcf8, cfg_addr(bus, dev, fn, off));
    outl(0xcfc, v);
}

uint32_t platform_pci_io_read(uint16_t port, unsigned size) {
    return size == 1 ? inb(port) : size == 2 ? inw(port) : inl(port);
}

void platform_pci_io_write(uint16_t port, unsigned size, uint32_t v) {
    if (size == 1)
        outb(port, (uint8_t)v);
    else if (size == 2)
        outw(port, (uint16_t)v);
    else
        outl(port, v);
}
