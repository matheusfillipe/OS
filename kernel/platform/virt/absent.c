#include <kernel/lib.h>
#include <kernel/platform.h>

/* QEMU virt as we run it has no keyboard, mouse or display controller. */

void platform_input_init(void) { kprintf("input: none on this platform\n"); }

bool platform_fb_mode_fits(uint32_t w, uint32_t h) {
    (void)w, (void)h;
    return false;
}

bool platform_fb_set_mode(uint32_t w, uint32_t h, uint64_t *phys, uint64_t *vram_bytes) {
    (void)w, (void)h, (void)phys, (void)vram_bytes;
    return false;
}
