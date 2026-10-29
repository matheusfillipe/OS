#include <asm/io.h>
#include <kernel/lib.h>
#include <kernel/platform.h>

/* QEMU's fw_cfg interface: lets the test harness pass named values
 * (-fw_cfg name=opt/os/...,string=...) without a kernel command line.
 * Harmless on real hardware: the signature check fails and nothing is read. */

#define FW_CFG_SELECTOR 0x510
#define FW_CFG_DATA     0x511
#define FW_CFG_SIGNATURE 0x0000
#define FW_CFG_FILE_DIR  0x0019

/* The selector register takes a little-endian 16-bit write on x86; the
 * directory contents are big-endian. */
static void select(uint16_t key) { outw(FW_CFG_SELECTOR, key); }

static void read_bytes(void *buf, size_t len) {
    uint8_t *b = buf;
    for (size_t i = 0; i < len; i++)
        b[i] = inb(FW_CFG_DATA);
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

int fwcfg_read_file(const char *name, char *buf, size_t size) {
    char sig[4];
    select(FW_CFG_SIGNATURE);
    read_bytes(sig, 4);
    if (memcmp(sig, "QEMU", 4))
        return -1;
    uint8_t cnt[4];
    select(FW_CFG_FILE_DIR);
    read_bytes(cnt, 4);
    uint32_t count = be32(cnt);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t ent[64];
        read_bytes(ent, sizeof ent);
        if (strcmp((const char *)ent + 8, name))
            continue;
        uint32_t fsize = be32(ent);
        uint16_t key = (uint16_t)(ent[4] << 8 | ent[5]);
        size_t n = fsize < size - 1 ? fsize : size - 1;
        select(key);
        read_bytes(buf, n);
        buf[n] = 0;
        return (int)n;
    }
    return -1;
}
