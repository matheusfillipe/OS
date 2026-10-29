#include <asm/io.h>
#include <kernel/platform.h>

static uint8_t cmos(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

int64_t platform_rtc_read(void) {
    while (cmos(0x0a) & 0x80) /* update in progress */
        ;
    uint8_t s = cmos(0x00), mi = cmos(0x02), h = cmos(0x04), d = cmos(0x07), mo = cmos(0x08), y = cmos(0x09);
    uint8_t b = cmos(0x0b);
    if (!(b & 0x04)) { /* BCD */
#define BCD(x) (uint8_t)(((x) & 0x0f) + ((x) >> 4) * 10)
        s = BCD(s), mi = BCD(mi), d = BCD(d), mo = BCD(mo), y = BCD(y);
        h = (uint8_t)(BCD(h & 0x7f) | (h & 0x80));
#undef BCD
    }
    if (!(b & 0x02) && (h & 0x80)) /* 12-hour clock */
        h = (uint8_t)(((h & 0x7f) + 12) % 24);
    int64_t days = days_from_civil(2000 + y, mo, d);
    return days * 86400 + h * 3600 + mi * 60 + s;
}
