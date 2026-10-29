#include <asm/io.h>
#include <kernel/platform.h>

#define FW_CFG_SELECTOR 0x510
#define FW_CFG_DATA     0x511

/* The selector register takes a little-endian 16-bit write on x86. */
void fwcfg_select(uint16_t key) { outw(FW_CFG_SELECTOR, key); }

void fwcfg_read(void *buf, size_t len) {
    uint8_t *b = buf;
    for (size_t i = 0; i < len; i++)
        b[i] = inb(FW_CFG_DATA);
}
