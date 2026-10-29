#include <asm/io.h>
#include <kernel/arch.h>
#include <kernel/boot.h>
#include <kernel/lib.h>
#include <kernel/mm.h>

/* GICv3: one distributor for SPIs, a redistributor per CPU for its SGIs and
 * PPIs, and the CPU interface through ICC system registers. We find the
 * distributor and the redistributor ranges in the ACPI MADT, which the
 * firmware fills for whatever machine it boots, and every interrupt goes to
 * group 1 with one priority. */

#define GICD_CTLR      0x0000
#define GICD_TYPER     0x0004
#define GICD_IGROUPR   0x0080
#define GICD_ISENABLER 0x0100
#define GICD_ICENABLER 0x0180
#define GICD_IPRIORITYR 0x0400
#define GICD_IROUTER   0x6000
#define GICD_CTLR_RWP  (1u << 31)
#define GICD_CTLR_ARE  (1u << 4)
#define GICD_CTLR_G1   (1u << 1)

#define GICR_CTLR      0x0000
#define GICR_WAKER     0x0014
#define GICR_TYPER     0x0008
#define GICR_SGI       0x10000 /* SGI_base frame, after RD_base */
#define GICR_CTLR_RWP  (1u << 3)
#define WAKER_SLEEP    (1u << 1)
#define WAKER_ASLEEP   (1u << 2)
#define TYPER_VLPIS    (1u << 1)
#define TYPER_LAST     (1u << 4)

#define MADT_GICD 0x0c
#define MADT_GICR 0x0e

#define PRIORITY 0xa0
#define NR_IRQS  1020

struct madt {
    char signature[4];
    uint32_t length;
    uint8_t rev, checksum;
    char oem[6], oem_table[8];
    uint32_t oem_rev, creator, creator_rev;
    uint32_t lapic_addr;
    uint32_t flags;
    uint8_t entries[];
} __attribute__((packed));

static volatile uint8_t *gicd;
static struct {
    uint64_t base, len;
} gicr_ranges[8];
static int gicr_range_count;
static irq_handler_t handlers[NR_IRQS];

static void wait_rwp(volatile uint8_t *base, uint32_t bit) {
    while (mmio_read32(base) & bit)
        cpu_relax();
}

/* Aff3.Aff2.Aff1.Aff0, the form GICR_TYPER and GICD_IROUTER use. */
static uint64_t affinity(uint64_t mpidr) { return (mpidr & 0xffffff) | (mpidr >> 32 & 0xff) << 24; }

void gic_init(void) {
    struct madt *m = acpi_find_table("APIC", 0);
    if (!m)
        panic("gic: no MADT");
    uint64_t gicd_phys = 0;
    for (uint8_t *p = m->entries; p < (uint8_t *)m + m->length; p += p[1]) {
        if (p[0] == MADT_GICD) {
            memcpy(&gicd_phys, p + 8, 8);
            if (p[20] && p[20] < 3)
                panic("gic: GICv%u, we need GICv3", p[20]);
        } else if (p[0] == MADT_GICR && gicr_range_count < (int)ARRAY_LEN(gicr_ranges)) {
            uint32_t len;
            memcpy(&gicr_ranges[gicr_range_count].base, p + 4, 8);
            memcpy(&len, p + 12, 4);
            gicr_ranges[gicr_range_count].len = len;
            vmm_map_mmio(gicr_ranges[gicr_range_count].base, len, true);
            gicr_range_count++;
        }
    }
    if (!gicd_phys || !gicr_range_count)
        panic("gic: the MADT lists no GICv3 distributor and redistributors");
    vmm_map_mmio(gicd_phys, 0x10000, true);
    gicd = phys_to_virt(gicd_phys);

    mmio_write32(gicd + GICD_CTLR, 0);
    wait_rwp(gicd, GICD_CTLR_RWP);
    uint32_t lines = 32 * ((mmio_read32(gicd + GICD_TYPER) & 0x1f) + 1);
    if (lines > NR_IRQS)
        lines = NR_IRQS;
    for (uint32_t i = 32; i < lines; i += 32) {
        mmio_write32(gicd + GICD_ICENABLER + i / 8, ~0u);
        mmio_write32(gicd + GICD_IGROUPR + i / 8, ~0u);
    }
    for (uint32_t i = 32; i < lines; i += 4)
        mmio_write32(gicd + GICD_IPRIORITYR + i, PRIORITY * 0x01010101u);
    mmio_write32(gicd + GICD_CTLR, GICD_CTLR_ARE | GICD_CTLR_G1);
    wait_rwp(gicd, GICD_CTLR_RWP);
    kprintf("gic: GICv3, %u interrupt lines, %d redistributor range(s)\n", lines, gicr_range_count);
}

static volatile uint8_t *find_redistributor(uint64_t aff) {
    for (int i = 0; i < gicr_range_count; i++) {
        uint64_t p = gicr_ranges[i].base, end = p + gicr_ranges[i].len;
        while (p < end) {
            uint64_t typer = mmio_read64(phys_to_virt(p + GICR_TYPER));
            if (typer >> 32 == aff)
                return phys_to_virt(p);
            if (typer & TYPER_LAST)
                break;
            p += typer & TYPER_VLPIS ? 0x40000 : 0x20000;
        }
    }
    return NULL;
}

void gic_init_cpu(void) {
    struct cpu *c = this_cpu();
    volatile uint8_t *rd = find_redistributor(affinity(c->arch.mpidr));
    if (!rd)
        panic("gic: no redistributor for cpu %u (mpidr %lx)", c->id, c->arch.mpidr);
    c->arch.gicr = rd;
    mmio_write32(rd + GICR_WAKER, mmio_read32(rd + GICR_WAKER) & ~WAKER_SLEEP);
    while (mmio_read32(rd + GICR_WAKER) & WAKER_ASLEEP)
        cpu_relax();

    volatile uint8_t *sgi = rd + GICR_SGI;
    mmio_write32(sgi + GICD_ICENABLER, ~0u);
    wait_rwp(rd, GICR_CTLR_RWP);
    mmio_write32(sgi + GICD_IGROUPR, ~0u);
    for (uint32_t i = 0; i < 32; i += 4)
        mmio_write32(sgi + GICD_IPRIORITYR + i, PRIORITY * 0x01010101u);
    uint32_t enable = 0;
    for (uint32_t i = 0; i < 32; i++)
        if (handlers[i])
            enable |= 1u << i;
    mmio_write32(sgi + GICD_ISENABLER, enable);

    uint64_t sre;
    __asm__ volatile("mrs %0, icc_sre_el1" : "=r"(sre));
    __asm__ volatile("msr icc_sre_el1, %0; isb" ::"r"(sre | 1));
    __asm__ volatile("msr icc_pmr_el1, %0; msr icc_bpr1_el1, xzr; msr icc_igrpen1_el1, %1; isb" ::"r"(0xffUL),
                     "r"(1UL));
}

void gic_send_sgi(struct cpu *c, unsigned intid) {
    uint64_t m = c->arch.mpidr, aff0 = m & 0xff;
    uint64_t v = 1ULL << (aff0 & 15) | (m >> 8 & 0xff) << 16 | (uint64_t)intid << 24 | (m >> 16 & 0xff) << 32 |
                 (aff0 >> 4) << 44 | (m >> 32 & 0xff) << 48;
    /* Our memory writes must be visible before the target takes the SGI. */
    __asm__ volatile("dsb ishst; msr icc_sgi1r_el1, %0; isb" ::"r"(v) : "memory");
}

void irq_set_handler(unsigned irq, irq_handler_t handler) {
    if (irq >= NR_IRQS)
        panic("arm64: LPIs need the GIC ITS, which is not implemented yet");
    handlers[irq] = handler;
    if (irq < 32)
        return;
    mmio_write8(gicd + GICD_IPRIORITYR + irq, PRIORITY);
    mmio_write64(gicd + GICD_IROUTER + 8 * irq, affinity(cpus[0]->arch.mpidr));
    mmio_write32(gicd + (handler ? GICD_ISENABLER : GICD_ICENABLER) + irq / 32 * 4, 1u << (irq % 32));
}

void irq_eoi(void) {
    __asm__ volatile("msr icc_eoir1_el1, %0; isb" ::"r"((uint64_t)this_cpu()->arch.irq_intid) : "memory");
}

/* Interrupts stay masked here, so the INTID we keep for irq_eoi is this one's. */
void irq_dispatch(struct regs *r) {
    uint64_t intid;
    __asm__ volatile("mrs %0, icc_iar1_el1" : "=r"(intid)::"memory");
    if (intid >= 1020 && intid < 1024) /* spurious */
        return;
    this_cpu()->arch.irq_intid = (uint32_t)intid;
    if (intid < NR_IRQS && handlers[intid]) {
        handlers[intid](r);
        return;
    }
    irq_eoi();
    kprintf("unexpected interrupt %lu on cpu %u\n", intid, this_cpu()->id);
}

bool arch_msi_setup(const struct pci_dev *d, unsigned irq, uint64_t *addr, uint32_t *data) {
    (void)d, (void)irq, (void)addr, (void)data;
    panic("arm64: MSI through the GIC ITS is not implemented yet");
}
