#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Bochs/QEMU std VGA mode setting (bga.c), used by /dev/fb0. */
/* Finds the device (once). Fills its framebuffer address and memory size. */
bool bga_probe(uint64_t *phys, uint64_t *vram_bytes);
/* Would w x h at 32 bits per pixel work? */
bool bga_mode_fits(uint32_t w, uint32_t h);
/* Switches to w x h, 32 bits per pixel, pitch w * 4. */
bool bga_set_mode(uint32_t w, uint32_t h);
