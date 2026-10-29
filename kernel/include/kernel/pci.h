#pragma once
#include <stdbool.h>
#include <stdint.h>

struct pci_dev {
    uint8_t bus, dev, fn;
    uint16_t vendor, device;
    uint8_t class_code, subclass, prog_if;
    uint8_t irq_line, irq_pin;
    uint64_t bar[6];     /* decoded base addresses (memory or I/O port) */
    uint64_t bar_size[6];
    bool bar_is_io[6];
};

void pci_init(void);
int pci_count(void);
struct pci_dev *pci_get(int index);
struct pci_dev *pci_find(uint16_t vendor, uint16_t device);
uint32_t pci_read32(const struct pci_dev *d, uint8_t off);
void pci_write32(const struct pci_dev *d, uint8_t off, uint32_t v);
uint16_t pci_read16(const struct pci_dev *d, uint8_t off);
void pci_write16(const struct pci_dev *d, uint8_t off, uint16_t v);
/* Enables memory/I-O decoding and bus mastering (DMA). */
void pci_enable(const struct pci_dev *d);
/* Points the device's single MSI at interrupt `irq`; false if it has no MSI (caller should poll). */
bool pci_enable_msi(const struct pci_dev *d, unsigned irq);
