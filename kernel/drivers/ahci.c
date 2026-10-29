#include <kernel/arch.h>
#include <kernel/blkdev.h>
#include <kernel/boot.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/pci.h>
#include <kernel/proc.h>
#include <kernel/sched.h>

/* AHCI (SATA) host controllers: the ICH9 controller built into QEMU's q35
 * machine, and the SATA ports of nearly every PC since 2008. One command
 * slot per port, issued and polled to completion under the block layer's
 * per-disk lock: simple, and fast enough that the disk itself dominates.
 * (Interrupts and NCQ can come later without changing the interface.)
 *
 * Firmware (OVMF, SeaBIOS) leaves the controller running with command lists
 * in memory that now belongs to us, so every port is stopped before anything
 * else, including the CD-ROM ports we never use: a running port could
 * otherwise DMA a received FIS into a page the allocator has handed out. */

/* HBA (generic host control) registers */
#define HBA_CAP  0x00
#define HBA_GHC  0x04
#define HBA_IS   0x08
#define HBA_PI   0x0c
#define HBA_VS   0x10
#define HBA_CAP2 0x24
#define HBA_BOHC 0x28

#define CAP_S64A  (1u << 31) /* 64-bit DMA addresses */
#define CAP_SCLO  (1u << 24) /* command list override */
#define CAP2_BOH  (1u << 0)  /* BIOS/OS handoff */
#define GHC_IE    (1u << 1)
#define GHC_AE    (1u << 31)
#define BOHC_BOS  (1u << 0)
#define BOHC_OOS  (1u << 1)
#define BOHC_BB   (1u << 4)

/* Port registers, 0x80 bytes per port from 0x100 */
#define PX_CLB   0x00
#define PX_CLBU  0x04
#define PX_FB    0x08
#define PX_FBU   0x0c
#define PX_IS    0x10
#define PX_IE    0x14
#define PX_CMD   0x18
#define PX_TFD   0x20
#define PX_SIG   0x24
#define PX_SSTS  0x28
#define PX_SERR  0x30
#define PX_CI    0x38

#define CMD_ST   (1u << 0)
#define CMD_SUD  (1u << 1)
#define CMD_POD  (1u << 2)
#define CMD_CLO  (1u << 3)
#define CMD_FRE  (1u << 4)
#define CMD_FR   (1u << 14)
#define CMD_CR   (1u << 15)
#define CMD_ICC_ACTIVE (1u << 28)

#define IS_ERRORS 0x78000000u /* task file, host bus fatal, host bus data, interface fatal */
#define TFD_ERR  0x01
#define TFD_DRQ  0x08
#define TFD_BSY  0x80

#define SIG_ATA  0x00000101u
#define SSTS_DET_PRESENT 3

#define ATA_READ_DMA_EXT   0x25
#define ATA_WRITE_DMA_EXT  0x35
#define ATA_FLUSH_CACHE_EXT 0xea
#define ATA_IDENTIFY       0xec

#define FIS_H2D 0x27

/* One command moves at most this much; longer requests become several.
 * The PRDT has an entry per physically contiguous run (at worst per page). */
#define MAX_XFER  (4u << 20)
#define PRD_MAX   (4u << 20) /* byte count field is 22 bits */
#define NPRD      (MAX_XFER / PAGE_SIZE + 1)
#define CMD_TIMEOUT_MS 30000

struct cmd_header {
    uint16_t flags; /* CFL (FIS dwords) in 4:0, W(rite) bit 6 */
    uint16_t prdtl;
    volatile uint32_t prdbc;
    uint32_t ctba, ctbau;
    uint32_t reserved[4];
};

struct prd {
    uint32_t dba, dbau, reserved, dbc; /* dbc: byte count - 1 */
};

struct cmd_table {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t reserved[48];
    struct prd prdt[NPRD];
};

struct ahci_port {
    struct blkdev bd;
    volatile uint8_t *regs;
    struct cmd_header *cl; /* command list; only slot 0 is used */
    struct cmd_table *ct;
    uint64_t ct_phys;
    bool dma64;
    bool can_clo;
    int port;
};

static inline uint32_t rd(volatile uint8_t *base, uint32_t reg) { return *(volatile uint32_t *)(base + reg); }
static inline void wr(volatile uint8_t *base, uint32_t reg, uint32_t v) { *(volatile uint32_t *)(base + reg) = v; }

/* Waits until (reg & mask) == want. Register changes take micro- to
 * milliseconds; the timeout is coarse (timer ticks). */
static bool wait_reg(volatile uint8_t *base, uint32_t reg, uint32_t mask, uint32_t want, uint64_t ms) {
    uint64_t deadline = global_ticks + ms * HZ / 1000 + 2;
    for (uint32_t spins = 0;; spins++) {
        if ((rd(base, reg) & mask) == want)
            return true;
        if (global_ticks > deadline)
            return false;
        if (spins > 256)
            thread_yield();
        else
            cpu_relax();
    }
}

static void *dma_alloc(size_t bytes, uint64_t *phys) {
    size_t pages = ALIGN_UP(bytes, PAGE_SIZE) / PAGE_SIZE;
    uint64_t p = pmm_alloc(pages);
    if (!p)
        return NULL;
    void *v = phys_to_virt(p);
    memset(v, 0, pages * PAGE_SIZE);
    *phys = p;
    return v;
}

static bool port_stop(volatile uint8_t *pr) {
    wr(pr, PX_CMD, rd(pr, PX_CMD) & ~CMD_ST);
    bool ok = wait_reg(pr, PX_CMD, CMD_CR, 0, 500);
    wr(pr, PX_CMD, rd(pr, PX_CMD) & ~CMD_FRE);
    return wait_reg(pr, PX_CMD, CMD_FR, 0, 500) && ok;
}

static bool port_start(struct ahci_port *p) {
    volatile uint8_t *pr = p->regs;
    if (!wait_reg(pr, PX_CMD, CMD_CR, 0, 500))
        return false;
    wr(pr, PX_CMD, rd(pr, PX_CMD) | CMD_FRE);
    /* A device still busy from before (or from an error) would make the
     * HBA ignore our commands; command list override clears that. */
    if ((rd(pr, PX_TFD) & (TFD_BSY | TFD_DRQ)) && p->can_clo) {
        wr(pr, PX_CMD, rd(pr, PX_CMD) | CMD_CLO);
        wait_reg(pr, PX_CMD, CMD_CLO, 0, 500);
    }
    if (!wait_reg(pr, PX_TFD, TFD_BSY | TFD_DRQ, 0, 1000))
        return false;
    wr(pr, PX_CMD, rd(pr, PX_CMD) | CMD_ST);
    return true;
}

/* After an error the port stops processing commands until software clears
 * the error state and restarts it (AHCI 1.3, section 6.2.2). */
static void port_recover(struct ahci_port *p) {
    port_stop(p->regs);
    wr(p->regs, PX_SERR, 0xffffffff);
    wr(p->regs, PX_IS, 0xffffffff);
    if (!port_start(p))
        kprintf("ahci: port %d: could not restart after an error\n", p->port);
}

/* Fills slot 0's PRDT for `bytes` at kernel address `buf`, merging
 * physically contiguous pages. Returns the entry count or -errno. */
static int build_prdt(struct ahci_port *p, void *buf, size_t bytes) {
    uint64_t va = (uint64_t)buf;
    int n = 0;
    uint64_t run_start = 0, run_len = 0;
    while (bytes) {
        uint64_t pa = vmm_translate(vmm_kernel_pml4(), va);
        if (!pa)
            return -EFAULT;
        size_t chunk = PAGE_SIZE - (va & (PAGE_SIZE - 1));
        if (chunk > bytes)
            chunk = bytes;
        if (!p->dma64 && pa + chunk > (1ULL << 32))
            return -EIO; /* would need a bounce below 4 GiB: not on any HBA we know */
        if (run_len && run_start + run_len == pa && run_len + chunk <= PRD_MAX) {
            run_len += chunk;
        } else {
            if (run_len) {
                if (n == (int)NPRD)
                    return -EINVAL;
                p->ct->prdt[n++] = (struct prd){(uint32_t)run_start, (uint32_t)(run_start >> 32), 0,
                                                (uint32_t)run_len - 1};
            }
            run_start = pa;
            run_len = chunk;
        }
        va += chunk;
        bytes -= chunk;
    }
    if (run_len) {
        if (n == (int)NPRD)
            return -EINVAL;
        p->ct->prdt[n++] = (struct prd){(uint32_t)run_start, (uint32_t)(run_start >> 32), 0, (uint32_t)run_len - 1};
    }
    return n;
}

/* Issues one ATA command in slot 0 and polls it to completion. */
static int issue(struct ahci_port *p, uint8_t cmd, uint64_t lba, uint32_t count, void *buf, size_t bytes,
                 bool write) {
    volatile uint8_t *pr = p->regs;
    int nprd = 0;
    if (bytes) {
        nprd = build_prdt(p, buf, bytes);
        if (nprd < 0)
            return nprd;
    }
    uint8_t *f = p->ct->cfis;
    memset(f, 0, 20);
    f[0] = FIS_H2D;
    f[1] = 0x80; /* this FIS carries a command */
    f[2] = cmd;
    f[4] = (uint8_t)lba, f[5] = (uint8_t)(lba >> 8), f[6] = (uint8_t)(lba >> 16);
    f[7] = (cmd == ATA_READ_DMA_EXT || cmd == ATA_WRITE_DMA_EXT) ? 0x40 : 0; /* LBA addressing */
    f[8] = (uint8_t)(lba >> 24), f[9] = (uint8_t)(lba >> 32), f[10] = (uint8_t)(lba >> 40);
    f[12] = (uint8_t)count, f[13] = (uint8_t)(count >> 8);

    struct cmd_header *h = &p->cl[0];
    h->flags = 5 | (write ? (1u << 6) : 0); /* the H2D FIS is 5 dwords */
    h->prdtl = (uint16_t)nprd;
    h->prdbc = 0;
    h->ctba = (uint32_t)p->ct_phys;
    h->ctbau = (uint32_t)(p->ct_phys >> 32);

    if (!wait_reg(pr, PX_TFD, TFD_BSY | TFD_DRQ, 0, 1000)) {
        port_recover(p);
        return -EIO;
    }
    wr(pr, PX_IS, 0xffffffff);
    __atomic_thread_fence(__ATOMIC_SEQ_CST); /* table contents before the doorbell */
    wr(pr, PX_CI, 1);

    uint64_t deadline = global_ticks + (uint64_t)CMD_TIMEOUT_MS * HZ / 1000;
    int r = 0;
    for (uint32_t spins = 0;; spins++) {
        if (rd(pr, PX_IS) & IS_ERRORS) {
            r = -EIO;
            break;
        }
        if (!(rd(pr, PX_CI) & 1))
            break;
        if (global_ticks > deadline) {
            r = -ETIMEDOUT;
            break;
        }
        /* Short commands finish within a few microseconds of polling; for
         * longer ones let other threads run meanwhile. */
        if (spins > 256)
            thread_yield();
        else
            cpu_relax();
    }
    if (r == 0 && (rd(pr, PX_TFD) & TFD_ERR))
        r = -EIO;
    if (r < 0) {
        kprintf("ahci: port %d: command %02x at lba %lu failed (%s, tfd %x, is %x, serr %x)\n", p->port, cmd, lba,
                r == -ETIMEDOUT ? "timeout" : "error", rd(pr, PX_TFD), rd(pr, PX_IS), rd(pr, PX_SERR));
        port_recover(p);
    }
    return r;
}

static int ahci_rw(struct blkdev *bd, uint64_t lba, uint64_t count, void *buf, bool write) {
    struct ahci_port *p = bd->drv;
    if (lba + count > bd->sectors || lba + count < lba)
        return -EIO;
    uint8_t *b = buf;
    uint64_t max = MAX_XFER / bd->sector_size;
    while (count) {
        uint64_t n = count < max ? count : max;
        size_t bytes = n * bd->sector_size;
        int r = issue(p, write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT, lba, (uint32_t)n, b, bytes, write);
        if (r < 0)
            return r;
        lba += n;
        count -= n;
        b += bytes;
    }
    return 0;
}

static int ahci_flush(struct blkdev *bd) { return issue(bd->drv, ATA_FLUSH_CACHE_EXT, 0, 0, NULL, 0, false); }

/* ATA strings are big-endian 16-bit words, space padded. */
static void ata_string(char *out, const uint16_t *words, int nwords) {
    int k = 0;
    for (int i = 0; i < nwords; i++) {
        out[k++] = (char)(words[i] >> 8);
        out[k++] = (char)words[i];
    }
    while (k > 0 && (out[k - 1] == ' ' || out[k - 1] == 0))
        k--;
    out[k] = 0;
}

static void probe_port(volatile uint8_t *abar, int port, uint32_t cap) {
    volatile uint8_t *pr = abar + 0x100 + port * 0x80;
    uint32_t ssts = rd(pr, PX_SSTS), sig = rd(pr, PX_SIG);
    if ((ssts & 0xf) != SSTS_DET_PRESENT || sig != SIG_ATA)
        return; /* empty, or ATAPI (CD/DVD), port multiplier, ... */

    /* On the failure paths below the port's memory is deliberately leaked:
     * the HBA may still point at it, and a disk that fails here is rare. */
    struct ahci_port *p = kzalloc(sizeof *p);
    if (!p)
        return;
    p->regs = pr;
    p->port = port;
    p->dma64 = cap & CAP_S64A;
    p->can_clo = cap & CAP_SCLO;
    uint64_t cl_phys = 0;
    /* One page: the 1 KiB command list, then the 256-byte received-FIS area. */
    p->cl = dma_alloc(PAGE_SIZE, &cl_phys);
    p->ct = dma_alloc(sizeof(struct cmd_table), &p->ct_phys);
    if (!p->cl || !p->ct || (!p->dma64 && (cl_phys >> 32 || p->ct_phys >> 32))) {
        kprintf("ahci: port %d: no usable DMA memory\n", port);
        return;
    }
    uint64_t fb_phys = cl_phys + 0x400;
    wr(pr, PX_CLB, (uint32_t)cl_phys);
    wr(pr, PX_CLBU, (uint32_t)(cl_phys >> 32));
    wr(pr, PX_FB, (uint32_t)fb_phys);
    wr(pr, PX_FBU, (uint32_t)(fb_phys >> 32));
    wr(pr, PX_SERR, 0xffffffff);
    wr(pr, PX_IS, 0xffffffff);
    wr(pr, PX_IE, 0); /* polled */
    wr(pr, PX_CMD, rd(pr, PX_CMD) | CMD_SUD | CMD_POD | CMD_ICC_ACTIVE);
    if (!port_start(p)) {
        kprintf("ahci: port %d: device does not become ready\n", port);
        return;
    }

    uint16_t *id = kzalloc(512);
    if (!id || issue(p, ATA_IDENTIFY, 0, 0, id, 512, false) < 0) {
        kprintf("ahci: port %d: IDENTIFY failed\n", port);
        kfree(id);
        return;
    }
    if (!(id[83] & (1u << 10))) {
        kprintf("ahci: port %d: disk without 48-bit addressing, not supported\n", port);
        kfree(id);
        return;
    }
    p->bd.sectors = (uint64_t)id[100] | (uint64_t)id[101] << 16 | (uint64_t)id[102] << 32 | (uint64_t)id[103] << 48;
    p->bd.sector_size = 512;
    /* Word 106 valid (bits 15:14 = 01) with "logical sector longer than 256
     * words": words 117-118 give the size in 16-bit words (4Kn drives). */
    if ((id[106] & 0xc000) == 0x4000 && (id[106] & (1u << 12)))
        p->bd.sector_size = 2 * ((uint32_t)id[117] | (uint32_t)id[118] << 16);
    ata_string(p->bd.model, id + 27, 20);
    kfree(id);

    p->bd.rw = ahci_rw;
    p->bd.flush = ahci_flush;
    p->bd.drv = p;
    if (blkdev_register(&p->bd) < 0) {
        kprintf("ahci: port %d: cannot register the disk\n", port);
        return;
    }
    uint64_t mib = p->bd.sectors * p->bd.sector_size >> 20;
    if (mib >= 10240)
        kprintf("ahci: port %d: %lu GiB disk (%s), /dev/%s\n", port, mib >> 10, p->bd.model, p->bd.name);
    else
        kprintf("ahci: port %d: %lu MiB disk (%s), /dev/%s\n", port, mib, p->bd.model, p->bd.name);
}

static void bring_up(struct pci_dev *pd) {
    if (pd->bar_is_io[5] || !pd->bar[5]) {
        kprintf("ahci: %02x:%02x.%u has no ABAR\n", pd->bus, pd->dev, pd->fn);
        return;
    }
    pci_enable(pd);
    uint64_t size = pd->bar_size[5] > 0x1100 ? pd->bar_size[5] : 0x1100;
    vmm_map_mmio(pd->bar[5], size, true);
    volatile uint8_t *abar = phys_to_virt(pd->bar[5]);

    /* Ask the firmware to let go if it supports the handoff protocol. */
    if (rd(abar, HBA_CAP2) & CAP2_BOH) {
        wr(abar, HBA_BOHC, rd(abar, HBA_BOHC) | BOHC_OOS);
        wait_reg(abar, HBA_BOHC, BOHC_BOS, 0, 25);
        if (rd(abar, HBA_BOHC) & BOHC_BB)
            wait_reg(abar, HBA_BOHC, BOHC_BB, 0, 2000);
    }
    wr(abar, HBA_GHC, (rd(abar, HBA_GHC) | GHC_AE) & ~GHC_IE);
    uint32_t cap = rd(abar, HBA_CAP), pi = rd(abar, HBA_PI), vs = rd(abar, HBA_VS);
    kprintf("ahci: %04x:%04x at %lx, AHCI %x.%x, %u ports, %u command slots\n", pd->vendor, pd->device, pd->bar[5],
            vs >> 16, (vs >> 8) & 0xff, (cap & 0x1f) + 1, ((cap >> 8) & 0x1f) + 1);

    for (int i = 0; i < 32; i++)
        if ((pi & (1u << i)) && !port_stop(abar + 0x100 + i * 0x80))
            kprintf("ahci: port %d does not stop\n", i);
    wr(abar, HBA_IS, 0xffffffff);
    for (int i = 0; i < 32; i++)
        if (pi & (1u << i))
            probe_port(abar, i, cap);
}

void ahci_init(void) {
    for (int i = 0; i < pci_count(); i++) {
        struct pci_dev *d = pci_get(i);
        if (d->class_code == 0x01 && d->subclass == 0x06 && d->prog_if == 0x01)
            bring_up(d);
    }
}
