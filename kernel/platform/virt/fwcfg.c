#include "virt.h"
#include <kernel/boot.h>
#include <kernel/mm.h>
#include <kernel/platform.h>

#define FW_CFG_DATA     0x00
#define FW_CFG_SELECTOR 0x08

static volatile uint8_t *regs(void) {
    vmm_map_mmio(VIRT_FW_CFG, 0x1000, true);
    return phys_to_virt(VIRT_FW_CFG);
}

/* The MMIO selector register takes a big-endian 16-bit write. */
void fwcfg_select(uint16_t key) {
    *(volatile uint16_t *)(regs() + FW_CFG_SELECTOR) = __builtin_bswap16(key);
}

void fwcfg_read(void *buf, size_t len) {
    volatile uint8_t *data = regs() + FW_CFG_DATA;
    uint8_t *b = buf;
    for (size_t i = 0; i < len; i++)
        b[i] = *data;
}
