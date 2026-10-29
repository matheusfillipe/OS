#include <asm/io.h>
#include <kernel/lib.h>
#include <kernel/pci.h>
#include <kernel/platform.h>

/* Bochs/QEMU "std VGA" (PCI 1234:1111) mode setting through its dispi
 * registers (index on I/O port 0x1CE, data on 0x1CF). The firmware already
 * put it in a linear 32-bit mode for Limine; changing the size is just
 * disable, write width/height/depth, enable with the linear framebuffer on.
 * The framebuffer stays at the device's BAR0, so a program only has to map
 * it again at the new size. Real graphics cards keep the firmware's mode. */

#define DISPI_INDEX 0x1ce
#define DISPI_DATA  0x1cf

enum {
    DISPI_ID = 0,
    DISPI_XRES = 1,
    DISPI_YRES = 2,
    DISPI_BPP = 3,
    DISPI_ENABLE = 4,
    DISPI_VIRT_WIDTH = 6,
    DISPI_VIRT_HEIGHT = 7,
    DISPI_X_OFFSET = 8,
    DISPI_Y_OFFSET = 9,
    DISPI_VIDEO_MEMORY_64K = 10,
};

#define DISPI_ENABLED     0x01
#define DISPI_LFB_ENABLED 0x40

static uint16_t dispi_read(uint16_t index) {
    outw(DISPI_INDEX, index);
    return inw(DISPI_DATA);
}

static void dispi_write(uint16_t index, uint16_t v) {
    outw(DISPI_INDEX, index);
    outw(DISPI_DATA, v);
}

static int state = -1; /* -1 not probed, 0 absent, 1 present */
static uint64_t lfb_phys, vram;

static bool bga_probe(uint64_t *phys, uint64_t *vram_bytes) {
    if (state < 0) {
        state = 0;
        struct pci_dev *d = pci_find(0x1234, 0x1111);
        uint16_t id = d ? dispi_read(DISPI_ID) : 0;
        /* 0xB0C0..0xB0C5 are the interface versions; 2 and up have the LFB */
        if (d && id >= 0xb0c2 && id <= 0xb0cf && !d->bar_is_io[0] && d->bar[0]) {
            lfb_phys = d->bar[0];
            uint64_t k64 = dispi_read(DISPI_VIDEO_MEMORY_64K);
            vram = k64 ? k64 * 65536 : d->bar_size[0];
            state = 1;
            kprintf("bga: std VGA %x, %lu MiB at %lx\n", id, vram >> 20, lfb_phys);
        }
    }
    if (state == 1) {
        if (phys)
            *phys = lfb_phys;
        if (vram_bytes)
            *vram_bytes = vram;
    }
    return state == 1;
}

bool platform_fb_mode_fits(uint32_t w, uint32_t h) {
    if (!bga_probe(NULL, NULL))
        return false;
    /* within what the device accepts, and in its memory */
    return w >= 320 && h >= 200 && w <= 2560 && h <= 1600 && (w % 8) == 0 && (uint64_t)w * h * 4 <= vram;
}

bool platform_fb_set_mode(uint32_t w, uint32_t h, uint64_t *phys, uint64_t *vram_bytes) {
    if (!platform_fb_mode_fits(w, h) || !bga_probe(phys, vram_bytes))
        return false;
    dispi_write(DISPI_ENABLE, 0);
    dispi_write(DISPI_XRES, (uint16_t)w);
    dispi_write(DISPI_YRES, (uint16_t)h);
    dispi_write(DISPI_BPP, 32);
    dispi_write(DISPI_VIRT_WIDTH, (uint16_t)w);
    dispi_write(DISPI_VIRT_HEIGHT, (uint16_t)h);
    dispi_write(DISPI_X_OFFSET, 0);
    dispi_write(DISPI_Y_OFFSET, 0);
    dispi_write(DISPI_ENABLE, DISPI_ENABLED | DISPI_LFB_ENABLED);
    /* the device may round or refuse: believe what it reports */
    return dispi_read(DISPI_XRES) == w && dispi_read(DISPI_YRES) == h;
}
