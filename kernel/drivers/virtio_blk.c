#include <kernel/arch.h>
#include <kernel/blkdev.h>
#include <kernel/boot.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/pci.h>
#include <kernel/platform.h>
#include <kernel/proc.h>
#include <kernel/sched.h>

/* virtio-blk through the legacy (virtio 0.9.5) PCI interface: I/O port
 * registers in BAR0 and a single virtqueue. QEMU, KVM clouds and most other
 * hypervisors offer it as a transitional device (1af4:1001). Like the AHCI
 * driver it runs one request at a time and polls the used ring, with device
 * interrupts suppressed. */

#define VIRTIO_VENDOR       0x1af4
#define VIRTIO_BLK_LEGACY   0x1001

/* legacy register block (no MSI-X, so device config starts at 0x14) */
#define VP_DEVICE_FEATURES 0x00
#define VP_GUEST_FEATURES  0x04
#define VP_QUEUE_PFN       0x08
#define VP_QUEUE_SIZE      0x0c
#define VP_QUEUE_SELECT    0x0e
#define VP_QUEUE_NOTIFY    0x10
#define VP_STATUS          0x12
#define VP_ISR             0x13
#define VP_CONFIG          0x14

#define STATUS_ACK      1
#define STATUS_DRIVER   2
#define STATUS_DRIVER_OK 4
#define STATUS_FAILED   0x80

#define BLK_F_RO       (1u << 5)
#define BLK_F_BLK_SIZE (1u << 6)
#define BLK_F_FLUSH    (1u << 9)

#define BLK_T_IN    0
#define BLK_T_OUT   1
#define BLK_T_FLUSH 4

#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2
#define VRING_AVAIL_F_NO_INTERRUPT 1

#define MAX_XFER (1u << 20) /* per request; longer transfers become several */
#define TIMEOUT_MS 30000

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags, next;
};

struct vring_avail {
    uint16_t flags;
    volatile uint16_t idx;
    uint16_t ring[];
};

struct vring_used_elem {
    uint32_t id, len;
};

struct vring_used {
    volatile uint16_t flags;
    volatile uint16_t idx;
    struct vring_used_elem ring[];
};

struct blk_req_hdr {
    uint32_t type, reserved;
    uint64_t sector; /* always in 512-byte units */
};

struct vblk {
    struct blkdev bd;
    uint16_t io;
    uint16_t qsize;
    uint16_t avail_idx, used_seen;
    struct vring_desc *desc;
    struct vring_avail *avail;
    struct vring_used *used;
    struct blk_req_hdr *hdr; /* request header and status byte, in one DMA page */
    volatile uint8_t *status;
    uint64_t hdr_phys;
    bool can_flush;
};

static uint32_t reg_read(struct vblk *v, uint16_t reg, unsigned size) {
    return platform_pci_io_read((uint16_t)(v->io + reg), size);
}

static void reg_write(struct vblk *v, uint16_t reg, unsigned size, uint32_t val) {
    platform_pci_io_write((uint16_t)(v->io + reg), size, val);
}

static uint64_t config64(struct vblk *v, uint16_t off) {
    return (uint64_t)reg_read(v, VP_CONFIG + off, 4) | (uint64_t)reg_read(v, VP_CONFIG + off + 4, 4) << 32;
}

/* Runs one request: header, the data segments of `buf`, status byte. */
static int request(struct vblk *v, uint32_t type, uint64_t sector, void *buf, size_t bytes, bool write) {
    v->hdr->type = type;
    v->hdr->reserved = 0;
    v->hdr->sector = sector;
    *v->status = 0xff;

    int n = 0;
    v->desc[n++] = (struct vring_desc){v->hdr_phys, sizeof *v->hdr, VRING_DESC_F_NEXT, 1};
    uint64_t va = (uint64_t)buf;
    while (bytes) {
        uint64_t pa = vmm_translate(vmm_kernel_pgd(), va);
        if (!pa)
            return -EFAULT;
        size_t chunk = PAGE_SIZE - (va & (PAGE_SIZE - 1));
        if (chunk > bytes)
            chunk = bytes;
        struct vring_desc *prev = &v->desc[n - 1];
        if (n > 1 && prev->addr + prev->len == pa) {
            prev->len += (uint32_t)chunk; /* physically contiguous: same segment */
        } else {
            if (n >= v->qsize - 1)
                return -EINVAL; /* callers keep requests small enough */
            v->desc[n] = (struct vring_desc){pa, (uint32_t)chunk,
                                             (uint16_t)(VRING_DESC_F_NEXT | (write ? 0 : VRING_DESC_F_WRITE)),
                                             (uint16_t)(n + 1)};
            n++;
        }
        va += chunk;
        bytes -= chunk;
    }
    v->desc[n] = (struct vring_desc){v->hdr_phys + 64, 1, VRING_DESC_F_WRITE, 0};

    v->avail->ring[v->avail_idx % v->qsize] = 0; /* the chain starts at descriptor 0 */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    v->avail->idx = ++v->avail_idx;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    reg_write(v, VP_QUEUE_NOTIFY, 2, 0);

    uint64_t deadline = global_ticks + (uint64_t)TIMEOUT_MS * HZ / 1000;
    for (uint32_t spins = 0; v->used->idx == v->used_seen; spins++) {
        if (global_ticks > deadline) {
            /* The descriptors still belong to the device; keep them out of
             * reuse by marking the disk broken (requests fail from now on). */
            kprintf("virtio-blk: %s: request timed out\n", v->bd.name);
            v->qsize = 0;
            return -ETIMEDOUT;
        }
        if (spins > 256)
            thread_yield();
        else
            cpu_relax();
    }
    v->used_seen++;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return *v->status == 0 ? 0 : -EIO;
}

static int vblk_rw(struct blkdev *bd, uint64_t lba, uint64_t count, void *buf, bool write) {
    struct vblk *v = bd->drv;
    if (!v->qsize)
        return -EIO;
    if (lba + count > bd->sectors || lba + count < lba)
        return -EIO;
    uint8_t *b = buf;
    /* Bound each request by size and, for scattered buffers, by the number
     * of descriptors: one per page at worst, plus header and status. */
    uint64_t max_bytes = MAX_XFER;
    if ((uint64_t)(v->qsize - 3) * PAGE_SIZE < max_bytes)
        max_bytes = (uint64_t)(v->qsize - 3) * PAGE_SIZE;
    uint64_t max = max_bytes / bd->sector_size;
    while (count) {
        uint64_t n = count < max ? count : max;
        size_t bytes = n * bd->sector_size;
        int r = request(v, write ? BLK_T_OUT : BLK_T_IN, lba * (bd->sector_size / 512), b, bytes, write);
        if (r < 0) {
            kprintf("virtio-blk: %s: %s at sector %lu failed\n", bd->name, write ? "write" : "read", lba);
            return r;
        }
        lba += n;
        count -= n;
        b += bytes;
    }
    return 0;
}

static int vblk_flush(struct blkdev *bd) {
    struct vblk *v = bd->drv;
    if (!v->qsize)
        return -EIO;
    return v->can_flush ? request(v, BLK_T_FLUSH, 0, NULL, 0, false) : 0;
}

static void bring_up(struct pci_dev *pd) {
    if (!pd->bar_is_io[0]) {
        kprintf("virtio-blk: %02x:%02x.%u has no legacy I/O interface\n", pd->bus, pd->dev, pd->fn);
        return;
    }
    struct vblk *v = kzalloc(sizeof *v);
    if (!v)
        return;
    pci_enable(pd);
    v->io = (uint16_t)pd->bar[0];
    reg_write(v, VP_STATUS, 1, 0); /* reset */
    reg_write(v, VP_STATUS, 1, STATUS_ACK);
    reg_write(v, VP_STATUS, 1, STATUS_ACK | STATUS_DRIVER);
    uint32_t features = reg_read(v, VP_DEVICE_FEATURES, 4);
    uint32_t want = features & (BLK_F_FLUSH | BLK_F_BLK_SIZE | BLK_F_RO);
    reg_write(v, VP_GUEST_FEATURES, 4, want);
    v->can_flush = want & BLK_F_FLUSH;

    reg_write(v, VP_QUEUE_SELECT, 2, 0);
    uint16_t qsize = (uint16_t)reg_read(v, VP_QUEUE_SIZE, 2);
    if (qsize < 4) {
        reg_write(v, VP_STATUS, 1, STATUS_FAILED);
        kprintf("virtio-blk: no usable queue\n");
        return;
    }
    /* Legacy layout: descriptors and the available ring, then the used ring
     * on the next page boundary. The queue address is a page number. */
    size_t avail_off = 16u * qsize;
    size_t used_off = ALIGN_UP(avail_off + 6 + 2u * qsize, PAGE_SIZE);
    size_t ring_bytes = used_off + ALIGN_UP(6 + 8u * qsize, PAGE_SIZE);
    uint64_t ring_phys = pmm_alloc(ring_bytes / PAGE_SIZE), hdr_phys = pmm_alloc(1);
    if (!ring_phys || !hdr_phys || (ring_phys >> 12) >> 32) {
        reg_write(v, VP_STATUS, 1, STATUS_FAILED);
        kprintf("virtio-blk: no memory for the queue\n");
        return;
    }
    uint8_t *ring = phys_to_virt(ring_phys);
    memset(ring, 0, ring_bytes);
    v->qsize = qsize;
    v->desc = (struct vring_desc *)ring;
    v->avail = (struct vring_avail *)(ring + avail_off);
    v->used = (struct vring_used *)(ring + used_off);
    v->avail->flags = VRING_AVAIL_F_NO_INTERRUPT; /* we poll */
    v->hdr_phys = hdr_phys;
    v->hdr = phys_to_virt(hdr_phys);
    v->status = (volatile uint8_t *)v->hdr + 64;
    reg_write(v, VP_QUEUE_PFN, 4, (uint32_t)(ring_phys >> 12));
    reg_write(v, VP_STATUS, 1, STATUS_ACK | STATUS_DRIVER | STATUS_DRIVER_OK);

    v->bd.sector_size = 512;
    if (want & BLK_F_BLK_SIZE) {
        uint32_t bs = reg_read(v, VP_CONFIG + 0x14, 4);
        if (bs >= 512 && bs <= 4096 && !(bs & (bs - 1)))
            v->bd.sector_size = bs;
    }
    v->bd.sectors = config64(v, 0) / (v->bd.sector_size / 512);
    ksnprintf(v->bd.model, sizeof v->bd.model, "virtio%s", (want & BLK_F_RO) ? ", read-only" : "");
    v->bd.rw = vblk_rw;
    v->bd.flush = vblk_flush;
    v->bd.drv = v;
    (void)reg_read(v, VP_ISR, 1); /* clear any pending interrupt */
    if (blkdev_register(&v->bd) < 0) {
        kprintf("virtio-blk: cannot register the disk\n");
        return;
    }
    kprintf("virtio-blk: %02x:%02x.%u: %lu MiB disk (%s), /dev/%s\n", pd->bus, pd->dev, pd->fn,
            v->bd.sectors * v->bd.sector_size >> 20, v->bd.model, v->bd.name);
}

void virtio_blk_init(void) {
    for (int i = 0; i < pci_count(); i++) {
        struct pci_dev *d = pci_get(i);
        if (d->vendor == VIRTIO_VENDOR && d->device == VIRTIO_BLK_LEGACY)
            bring_up(d);
    }
}
