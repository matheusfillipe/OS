#include <asm/io.h>
#include <asm/msr.h>
#include <kernel/arch.h>
#include <kernel/boot.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>

/* Local APIC (x2APIC when the CPU has it, xAPIC MMIO otherwise), I/O APIC,
 * and the periodic scheduler tick. The legacy 8259 PIC is masked for good. */

#define MSR_APIC_BASE      0x1b
#define APIC_BASE_ENABLE   (1ULL << 11)
#define APIC_BASE_X2APIC   (1ULL << 10)

#define LAPIC_ID       0x020
#define LAPIC_EOI      0x0b0
#define LAPIC_SVR      0x0f0
#define LAPIC_ICR_LO   0x300
#define LAPIC_ICR_HI   0x310
#define LAPIC_LVT_TMR  0x320
#define LAPIC_TMR_INIT 0x380
#define LAPIC_TMR_CUR  0x390
#define LAPIC_TMR_DIV  0x3e0

#define LVT_MASKED   (1u << 16)
#define LVT_PERIODIC (1u << 17)

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

struct ioapic {
    volatile uint32_t *base;
    uint32_t gsi_base, gsi_count;
};

struct iso {
    uint8_t source;
    uint32_t gsi;
    uint16_t flags;
};

static bool x2apic;
static volatile uint32_t *lapic_mmio;
static struct ioapic ioapics[8];
static int ioapic_count;
static struct iso isos[16];
static int iso_count;
static uint32_t ticks_per_period; /* APIC timer count per 1/HZ s, divider 16 */

static uint32_t lapic_read(uint32_t reg) {
    return x2apic ? (uint32_t)rdmsr(0x800 + (reg >> 4)) : lapic_mmio[reg / 4];
}

static void lapic_write(uint32_t reg, uint32_t v) {
    if (x2apic)
        wrmsr(0x800 + (reg >> 4), v);
    else
        lapic_mmio[reg / 4] = v;
}

uint32_t lapic_id(void) { return x2apic ? lapic_read(LAPIC_ID) : lapic_read(LAPIC_ID) >> 24; }

void lapic_eoi(void) { lapic_write(LAPIC_EOI, 0); }

void lapic_send_ipi(uint32_t apic_id, uint8_t vector) {
    if (x2apic) {
        wrmsr(0x800 + (LAPIC_ICR_LO >> 4), ((uint64_t)apic_id << 32) | vector);
    } else {
        lapic_write(LAPIC_ICR_HI, apic_id << 24);
        lapic_write(LAPIC_ICR_LO, vector);
        while (lapic_read(LAPIC_ICR_LO) & (1u << 12)) /* delivery pending */
            cpu_relax();
    }
}

static uint32_t ioapic_read(struct ioapic *io, uint32_t reg) {
    io->base[0] = reg;
    return io->base[4];
}

static void ioapic_write(struct ioapic *io, uint32_t reg, uint32_t v) {
    io->base[0] = reg;
    io->base[4] = v;
}

static struct ioapic *ioapic_for(uint32_t gsi) {
    for (int i = 0; i < ioapic_count; i++)
        if (gsi >= ioapics[i].gsi_base && gsi < ioapics[i].gsi_base + ioapics[i].gsi_count)
            return &ioapics[i];
    return NULL;
}

void ioapic_route(uint32_t gsi, uint8_t vector, uint32_t dest_apic, bool level, bool active_low) {
    struct ioapic *io = ioapic_for(gsi);
    if (!io)
        panic("ioapic: no I/O APIC handles GSI %u", gsi);
    uint32_t pin = gsi - io->gsi_base;
    uint32_t lo = vector | (level ? 1u << 15 : 0) | (active_low ? 1u << 13 : 0);
    ioapic_write(io, 0x10 + pin * 2 + 1, dest_apic << 24);
    ioapic_write(io, 0x10 + pin * 2, lo);
}

uint32_t irq_to_gsi(uint8_t isa_irq, bool *level, bool *active_low) {
    *level = false;
    *active_low = false;
    for (int i = 0; i < iso_count; i++) {
        if (isos[i].source != isa_irq)
            continue;
        uint16_t pol = isos[i].flags & 3, trig = (isos[i].flags >> 2) & 3;
        *active_low = pol == 3;
        *level = trig == 3;
        return isos[i].gsi;
    }
    return isa_irq;
}

static void pic_disable(void) {
    /* Remap to 0x20/0x28 so a stray legacy IRQ can't masquerade as an
     * exception, then mask every line. */
    outb(0x20, 0x11); io_wait();
    outb(0xa0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait();
    outb(0xa1, 0x28); io_wait();
    outb(0x21, 0x04); io_wait();
    outb(0xa1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait();
    outb(0xa1, 0x01); io_wait();
    outb(0x21, 0xff);
    outb(0xa1, 0xff);
}

static void parse_madt(void) {
    struct madt *m = acpi_find_table("APIC", 0);
    if (!m)
        panic("apic: no MADT");
    uint64_t lapic_phys = m->lapic_addr;
    for (uint8_t *p = m->entries; p < (uint8_t *)m + m->length; p += p[1]) {
        switch (p[0]) {
        case 1: { /* I/O APIC */
            if (ioapic_count == (int)ARRAY_LEN(ioapics))
                break;
            uint32_t addr, gsi_base;
            memcpy(&addr, p + 4, 4);
            memcpy(&gsi_base, p + 8, 4);
            vmm_map_mmio(addr, PAGE_SIZE, true);
            struct ioapic *io = &ioapics[ioapic_count++];
            io->base = phys_to_virt(addr);
            io->gsi_base = gsi_base;
            io->gsi_count = ((ioapic_read(io, 1) >> 16) & 0xff) + 1;
            for (uint32_t i = 0; i < io->gsi_count; i++)
                ioapic_write(io, 0x10 + i * 2, LVT_MASKED);
            break;
        }
        case 2: /* interrupt source override */
            if (iso_count < (int)ARRAY_LEN(isos)) {
                isos[iso_count].source = p[3];
                memcpy(&isos[iso_count].gsi, p + 4, 4);
                memcpy(&isos[iso_count].flags, p + 8, 2);
                iso_count++;
            }
            break;
        case 5: /* 64-bit local APIC address override */
            memcpy(&lapic_phys, p + 4, 8);
            break;
        }
    }
    if (!x2apic) {
        vmm_map_mmio(lapic_phys, PAGE_SIZE, true);
        lapic_mmio = phys_to_virt(lapic_phys);
    }
}

/* Count APIC timer ticks across 10 ms of PIT channel 2. */
static uint32_t calibrate_timer(void) {
    const uint16_t pit_10ms = 11932; /* 1.193182 MHz / 100 */
    outb(0x61, (inb(0x61) & ~0x02) | 0x01); /* gate on, speaker off */
    outb(0x43, 0xb0);                         /* channel 2, lo/hi, mode 0 */
    outb(0x42, pit_10ms & 0xff);
    outb(0x42, pit_10ms >> 8);
    uint8_t g = inb(0x61);
    outb(0x61, g & ~1); /* restart the count with a rising gate edge */
    outb(0x61, g | 1);

    lapic_write(LAPIC_TMR_DIV, 0x3); /* divide by 16 */
    lapic_write(LAPIC_TMR_INIT, 0xffffffff);
    while (!(inb(0x61) & 0x20))
        ;
    uint32_t elapsed = 0xffffffff - lapic_read(LAPIC_TMR_CUR);
    lapic_write(LAPIC_TMR_INIT, 0);
    return elapsed;
}

static void enable_lapic_mode(void) {
    uint64_t base = rdmsr(MSR_APIC_BASE) | APIC_BASE_ENABLE;
    if (x2apic)
        base |= APIC_BASE_X2APIC;
    wrmsr(MSR_APIC_BASE, base);
}

void apic_init(void) {
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    x2apic = c & (1u << 21);
    enable_lapic_mode();
    pic_disable();
    parse_madt();
    lapic_write(LAPIC_SVR, 0x100 | VEC_SPURIOUS);
    uint32_t per_10ms = calibrate_timer();
    ticks_per_period = per_10ms * 100 / HZ;
    kprintf("apic: %s, %d I/O APIC(s), timer %u kHz\n", x2apic ? "x2APIC" : "xAPIC", ioapic_count,
            per_10ms / 10 * 16);
}

void lapic_init_cpu(void) {
    enable_lapic_mode();
    lapic_write(LAPIC_SVR, 0x100 | VEC_SPURIOUS);
    lapic_write(LAPIC_TMR_DIV, 0x3);
    lapic_write(LAPIC_LVT_TMR, VEC_TIMER | LVT_PERIODIC);
    lapic_write(LAPIC_TMR_INIT, ticks_per_period);
}
