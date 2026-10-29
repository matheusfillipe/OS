#include "virt.h"
#include <kernel/boot.h>
#include <kernel/mm.h>
#include <kernel/platform.h>

#define PL031_DR 0x00

/* The PL031 counts seconds since the Unix epoch. */
int64_t platform_rtc_read(void) {
    vmm_map_mmio(VIRT_RTC, 0x1000, true);
    return *(volatile uint32_t *)phys_to_virt(VIRT_RTC + PL031_DR);
}
