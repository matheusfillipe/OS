#include <kernel/boot.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/net.h>
#include <kernel/pci.h>

/* Intel 8254x/8257x ("e1000") Gigabit Ethernet: the 82540EM that QEMU,
 * VirtualBox and VMware emulate, and the e1000e family (82574L, I217/I219
 * share the descriptor format). Polled by the network thread: no interrupt
 * routing needed, which keeps it working before ACPI _PRT parsing exists. */

#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_EERD   0x0014
#define REG_ICR    0x00c0
#define REG_IMC    0x00d8
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_TIPG   0x0410
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_MTA    0x5200
#define REG_RAL0   0x5400
#define REG_RAH0   0x5404

#define CTRL_SLU  (1u << 6)
#define CTRL_RST  (1u << 26)
#define RCTL_EN   (1u << 1)
#define RCTL_BAM  (1u << 15)
#define RCTL_SECRC (1u << 26)
#define TCTL_EN   (1u << 1)
#define TCTL_PSP  (1u << 3)
#define TX_CMD_EOP  0x01
#define TX_CMD_IFCS 0x02
#define TX_CMD_RS   0x08
#define DESC_DD     0x01

#define NRX 64
#define NTX 64
#define BUF 2048

struct rx_desc {
    uint64_t addr;
    uint16_t length, csum;
    uint8_t status, errors;
    uint16_t special;
} __attribute__((packed));

struct tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t cso, cmd, status, css;
    uint16_t special;
} __attribute__((packed));

struct e1000 {
    volatile uint8_t *mmio;
    struct rx_desc *rx;
    struct tx_desc *tx;
    uint8_t *rx_buf[NRX], *tx_buf[NTX];
    uint32_t rx_cur, tx_cur;
    struct netif nif;
};

static inline uint32_t rd(struct e1000 *e, uint32_t reg) { return *(volatile uint32_t *)(e->mmio + reg); }
static inline void wr(struct e1000 *e, uint32_t reg, uint32_t v) { *(volatile uint32_t *)(e->mmio + reg) = v; }

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

static int e1000_tx(struct netif *nif, const void *frame, size_t len) {
    struct e1000 *e = nif->drv;
    if (len > BUF)
        return -1;
    struct tx_desc *d = &e->tx[e->tx_cur];
    if (d->cmd && !(d->status & DESC_DD))
        return -1; /* ring full: the oldest descriptor is still in flight */
    memcpy(e->tx_buf[e->tx_cur], frame, len);
    d->length = (uint16_t)(len < 60 ? 60 : len); /* pad runts */
    if (len < 60)
        memset(e->tx_buf[e->tx_cur] + len, 0, 60 - len);
    d->cmd = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
    d->status = 0;
    e->tx_cur = (e->tx_cur + 1) % NTX;
    barrier();
    wr(e, REG_TDT, e->tx_cur);
    nif->tx_packets++;
    return 0;
}

/* The STATUS register's LU bit: link up (in QEMU, `set_link` turns it off). */
static bool e1000_link(struct netif *nif) {
    struct e1000 *e = nif->drv;
    return (rd(e, REG_STATUS) & 2) != 0;
}

static int e1000_poll(struct netif *nif) {
    struct e1000 *e = nif->drv;
    int n = 0;
    (void)rd(e, REG_ICR); /* clear any latched causes */
    while (e->rx[e->rx_cur].status & DESC_DD) {
        struct rx_desc *d = &e->rx[e->rx_cur];
        if (!d->errors && (d->status & 0x02 /* EOP */))
            net_rx(nif, e->rx_buf[e->rx_cur], d->length);
        d->status = 0;
        uint32_t done = e->rx_cur;
        e->rx_cur = (e->rx_cur + 1) % NRX;
        wr(e, REG_RDT, done);
        nif->rx_packets++;
        if (++n == NRX)
            break;
    }
    return n;
}

static uint16_t eeprom_read(struct e1000 *e, uint8_t addr) {
    wr(e, REG_EERD, 1u | ((uint32_t)addr << 8));
    for (int i = 0; i < 100000; i++) {
        uint32_t v = rd(e, REG_EERD);
        if (v & (1u << 4))
            return (uint16_t)(v >> 16);
    }
    return 0;
}

/* Chips whose register model this driver matches. Laptop I217/I219 parts
 * need PHY/ULP handling first and are added with the real-hardware work. */
static const uint16_t supported[] = {
    0x100e, /* 82540EM: QEMU, VirtualBox, VMware default */
    0x100f, /* 82545EM: VMware */
    0x1004, /* 82543GC */
    0x10d3, /* 82574L: QEMU e1000e, VMware e1000e */
};

static void bring_up(struct pci_dev *pd) {
    struct e1000 *e = kzalloc(sizeof *e);
    KASSERT(e);
    pci_enable(pd);
    vmm_map_mmio(pd->bar[0], pd->bar_size[0] ? pd->bar_size[0] : 0x20000, true);
    e->mmio = phys_to_virt(pd->bar[0]);

    wr(e, REG_IMC, 0xffffffff);
    wr(e, REG_CTRL, rd(e, REG_CTRL) | CTRL_RST);
    for (volatile int i = 0; i < 1000000 && (rd(e, REG_CTRL) & CTRL_RST); i++)
        ;
    wr(e, REG_IMC, 0xffffffff);
    (void)rd(e, REG_ICR);

    uint32_t ral = rd(e, REG_RAL0), rah = rd(e, REG_RAH0);
    if (rah & (1u << 31)) {
        for (int i = 0; i < 4; i++)
            e->nif.mac[i] = (uint8_t)(ral >> (8 * i));
        e->nif.mac[4] = (uint8_t)rah;
        e->nif.mac[5] = (uint8_t)(rah >> 8);
    } else {
        for (int i = 0; i < 3; i++) {
            uint16_t w = eeprom_read(e, (uint8_t)i);
            e->nif.mac[2 * i] = (uint8_t)w;
            e->nif.mac[2 * i + 1] = (uint8_t)(w >> 8);
        }
        wr(e, REG_RAL0, e->nif.mac[0] | e->nif.mac[1] << 8 | e->nif.mac[2] << 16 | (uint32_t)e->nif.mac[3] << 24);
        wr(e, REG_RAH0, e->nif.mac[4] | e->nif.mac[5] << 8 | (1u << 31));
    }
    for (int i = 0; i < 128; i++)
        wr(e, REG_MTA + i * 4, 0);

    uint64_t rx_phys = 0, tx_phys = 0;
    e->rx = dma_alloc(sizeof(struct rx_desc) * NRX, &rx_phys);
    e->tx = dma_alloc(sizeof(struct tx_desc) * NTX, &tx_phys);
    KASSERT(e->rx && e->tx);
    for (int i = 0; i < NRX; i++) {
        uint64_t p = 0;
        e->rx_buf[i] = dma_alloc(BUF, &p);
        KASSERT(e->rx_buf[i]);
        e->rx[i].addr = p;
    }
    for (int i = 0; i < NTX; i++) {
        uint64_t p = 0;
        e->tx_buf[i] = dma_alloc(BUF, &p);
        KASSERT(e->tx_buf[i]);
        e->tx[i].addr = p;
    }
    wr(e, REG_RDBAL, (uint32_t)rx_phys);
    wr(e, REG_RDBAH, (uint32_t)(rx_phys >> 32));
    wr(e, REG_RDLEN, sizeof(struct rx_desc) * NRX);
    wr(e, REG_RDH, 0);
    wr(e, REG_RDT, NRX - 1);
    wr(e, REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC); /* 2 KiB buffers */

    wr(e, REG_TDBAL, (uint32_t)tx_phys);
    wr(e, REG_TDBAH, (uint32_t)(tx_phys >> 32));
    wr(e, REG_TDLEN, sizeof(struct tx_desc) * NTX);
    wr(e, REG_TDH, 0);
    wr(e, REG_TDT, 0);
    wr(e, REG_TCTL, TCTL_EN | TCTL_PSP | (0x10u << 4) | (0x40u << 12));
    wr(e, REG_TIPG, 0x0060200a);
    wr(e, REG_CTRL, rd(e, REG_CTRL) | CTRL_SLU);

    ksnprintf(e->nif.name, sizeof e->nif.name, "eth0");
    e->nif.tx = e1000_tx;
    e->nif.poll = e1000_poll;
    e->nif.link = e1000_link;
    e->nif.drv = e;
    kprintf("e1000: %04x at %lx, mac %02x:%02x:%02x:%02x:%02x:%02x, link %s\n", pd->device, pd->bar[0],
            e->nif.mac[0], e->nif.mac[1], e->nif.mac[2], e->nif.mac[3], e->nif.mac[4], e->nif.mac[5],
            (rd(e, REG_STATUS) & 2) ? "up" : "down");
    net_register(&e->nif);
}

void e1000_init(void) {
    for (int i = 0; i < pci_count(); i++) {
        struct pci_dev *d = pci_get(i);
        if (d->vendor != 0x8086 || d->class_code != 0x02)
            continue;
        for (size_t k = 0; k < ARRAY_LEN(supported); k++)
            if (d->device == supported[k]) {
                bring_up(d);
                return; /* one NIC is plenty for now */
            }
    }
}
