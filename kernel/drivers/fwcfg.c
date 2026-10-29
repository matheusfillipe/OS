#include <kernel/lib.h>
#include <kernel/platform.h>

/* QEMU's fw_cfg interface: lets the test harness pass named values
 * (-fw_cfg name=opt/os/...,string=...) without a kernel command line.
 * Harmless on real hardware: the signature check fails and nothing is read.
 * The directory contents are big-endian. */

#define FW_CFG_SIGNATURE 0x0000
#define FW_CFG_FILE_DIR  0x0019

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

int fwcfg_read_file(const char *name, char *buf, size_t size) {
    char sig[4];
    fwcfg_select(FW_CFG_SIGNATURE);
    fwcfg_read(sig, 4);
    if (memcmp(sig, "QEMU", 4))
        return -1;
    uint8_t cnt[4];
    fwcfg_select(FW_CFG_FILE_DIR);
    fwcfg_read(cnt, 4);
    uint32_t count = be32(cnt);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t ent[64];
        fwcfg_read(ent, sizeof ent);
        if (strcmp((const char *)ent + 8, name))
            continue;
        uint32_t fsize = be32(ent);
        uint16_t key = (uint16_t)(ent[4] << 8 | ent[5]);
        size_t n = fsize < size - 1 ? fsize : size - 1;
        fwcfg_select(key);
        fwcfg_read(buf, n);
        buf[n] = 0;
        return (int)n;
    }
    return -1;
}
