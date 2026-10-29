#include <asm/io.h>
#include <kernel/arch.h>
#include <kernel/lib.h>
#include <kernel/pci.h>

/* PCI configuration access through the legacy 0xCF8/0xCFC ports, which every
 * x86 PC still provides. ECAM (MCFG) access replaces it for PCIe extended
 * config space when a driver needs it. */

#define MAX_PCI 128

static struct pci_dev devs[MAX_PCI];
static int ndevs;

static uint32_t cfg_addr(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    return 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | (off & 0xfc);
}

static uint32_t raw_read(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    outl(0xcf8, cfg_addr(bus, dev, fn, off));
    return inl(0xcfc);
}

static void raw_write(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v) {
    outl(0xcf8, cfg_addr(bus, dev, fn, off));
    outl(0xcfc, v);
}

uint32_t pci_read32(const struct pci_dev *d, uint8_t off) { return raw_read(d->bus, d->dev, d->fn, off); }
void pci_write32(const struct pci_dev *d, uint8_t off, uint32_t v) { raw_write(d->bus, d->dev, d->fn, off, v); }

uint16_t pci_read16(const struct pci_dev *d, uint8_t off) {
    return (uint16_t)(pci_read32(d, off & ~3) >> ((off & 2) * 8));
}

void pci_write16(const struct pci_dev *d, uint8_t off, uint16_t v) {
    uint32_t w = pci_read32(d, off & ~3);
    int shift = (off & 2) * 8;
    w = (w & ~(0xffffu << shift)) | ((uint32_t)v << shift);
    pci_write32(d, off & ~3, w);
}

static void probe_bars(struct pci_dev *d) {
    for (int i = 0; i < 6; i++) {
        uint8_t off = (uint8_t)(0x10 + i * 4);
        uint32_t orig = pci_read32(d, off);
        if (orig & 1) { /* I/O space */
            pci_write32(d, off, 0xffffffff);
            uint32_t sz = pci_read32(d, off) & ~3u;
            pci_write32(d, off, orig);
            d->bar_is_io[i] = true;
            d->bar[i] = orig & ~3u;
            d->bar_size[i] = sz ? (~sz + 1) & 0xffff : 0;
            continue;
        }
        bool is64 = ((orig >> 1) & 3) == 2;
        uint32_t orig_hi = is64 ? pci_read32(d, (uint8_t)(off + 4)) : 0;
        pci_write32(d, off, 0xffffffff);
        uint32_t lo = pci_read32(d, off);
        uint32_t hi = 0xffffffff;
        if (is64) {
            pci_write32(d, (uint8_t)(off + 4), 0xffffffff);
            hi = pci_read32(d, (uint8_t)(off + 4));
            pci_write32(d, (uint8_t)(off + 4), orig_hi);
        }
        pci_write32(d, off, orig);
        uint64_t mask = ((uint64_t)hi << 32) | (lo & ~0xfu);
        d->bar[i] = ((uint64_t)orig_hi << 32) | (orig & ~0xfu);
        d->bar_size[i] = mask ? (~mask + 1) & (is64 ? ~0ULL : 0xffffffffULL) : 0;
        if (is64)
            i++; /* the next slot holds the high half */
    }
}

static void scan_fn(uint8_t bus, uint8_t dev, uint8_t fn) {
    uint32_t id = raw_read(bus, dev, fn, 0);
    if ((id & 0xffff) == 0xffff || ndevs == MAX_PCI)
        return;
    struct pci_dev *d = &devs[ndevs++];
    memset(d, 0, sizeof *d);
    d->bus = bus, d->dev = dev, d->fn = fn;
    d->vendor = id & 0xffff;
    d->device = id >> 16;
    uint32_t cls = raw_read(bus, dev, fn, 0x08);
    d->class_code = cls >> 24;
    d->subclass = (cls >> 16) & 0xff;
    d->prog_if = (cls >> 8) & 0xff;
    uint32_t irq = raw_read(bus, dev, fn, 0x3c);
    d->irq_line = irq & 0xff;
    d->irq_pin = (irq >> 8) & 0xff;
    uint8_t header = (raw_read(bus, dev, fn, 0x0c) >> 16) & 0x7f;
    if (header == 0)
        probe_bars(d);
}

void pci_init(void) {
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            uint32_t id = raw_read((uint8_t)bus, (uint8_t)dev, 0, 0);
            if ((id & 0xffff) == 0xffff)
                continue;
            bool multi = (raw_read((uint8_t)bus, (uint8_t)dev, 0, 0x0c) >> 16) & 0x80;
            for (int fn = 0; fn < (multi ? 8 : 1); fn++)
                scan_fn((uint8_t)bus, (uint8_t)dev, (uint8_t)fn);
        }
    }
    kprintf("pci: %d devices\n", ndevs);
    for (int i = 0; i < ndevs; i++) {
        struct pci_dev *d = &devs[i];
        kprintf("  %02x:%02x.%u %04x:%04x class %02x.%02x irq %u\n", d->bus, d->dev, d->fn, d->vendor, d->device,
                d->class_code, d->subclass, d->irq_line);
    }
}

int pci_count(void) { return ndevs; }
struct pci_dev *pci_get(int index) { return index >= 0 && index < ndevs ? &devs[index] : NULL; }

struct pci_dev *pci_find(uint16_t vendor, uint16_t device) {
    for (int i = 0; i < ndevs; i++)
        if (devs[i].vendor == vendor && devs[i].device == device)
            return &devs[i];
    return NULL;
}

void pci_enable(const struct pci_dev *d) {
    uint16_t cmd = pci_read16(d, 0x04);
    cmd |= 0x7; /* I/O space, memory space, bus master */
    cmd &= (uint16_t)~0x400; /* make sure legacy INTx is not disabled */
    pci_write16(d, 0x04, cmd);
}

/* Message-signalled interrupts avoid the legacy INTx routing, which needs
 * the ACPI _PRT tables we don't parse yet. */
bool pci_enable_msi(const struct pci_dev *d, uint8_t vector) {
    if (!(pci_read16(d, 0x06) & 0x10)) /* no capability list */
        return false;
    uint8_t ptr = pci_read32(d, 0x34) & 0xfc;
    for (int guard = 0; ptr && guard < 48; guard++) {
        uint32_t hdr = pci_read32(d, ptr);
        if ((hdr & 0xff) == 0x05) {
            uint16_t ctl = (uint16_t)(hdr >> 16);
            pci_write32(d, (uint8_t)(ptr + 4), arch_msi_address());
            if (ctl & 0x80) { /* 64-bit address */
                pci_write32(d, (uint8_t)(ptr + 8), 0);
                pci_write16(d, (uint8_t)(ptr + 12), vector);
            } else {
                pci_write16(d, (uint8_t)(ptr + 8), vector);
            }
            pci_write16(d, (uint8_t)(ptr + 2), (uint16_t)((ctl & ~0x70u) | 1)); /* one vector, enabled */
            pci_write16(d, 0x04, pci_read16(d, 0x04) | 0x400);                 /* INTx off */
            return true;
        }
        ptr = (hdr >> 8) & 0xfc;
    }
    return false;
}
