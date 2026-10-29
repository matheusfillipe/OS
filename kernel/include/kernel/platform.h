#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Boot log sink: the first and most reliable output the kernel has. */
bool platform_console_init(void);
void platform_console_write(const char *s, uint64_t len);
void platform_console_rx_init(void (*rx)(char c));

/* Text sent by the host (clipboard sharing in VMs), read without blocking. */
bool platform_hostlink_init(void);
int64_t platform_hostlink_read(void *buf, uint64_t len);

void platform_input_init(void);
int64_t platform_rtc_read(void);
void platform_reboot(void);
void platform_poweroff(void);
void platform_test_exit(uint8_t code);
uint32_t platform_pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
void platform_pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v);
/* Would w x h at 32 bits per pixel work on the display? */
bool platform_fb_mode_fits(uint32_t w, uint32_t h);
/* Switches to w x h, 32 bits per pixel, pitch w * 4; returns the framebuffer address and memory size. */
bool platform_fb_set_mode(uint32_t w, uint32_t h, uint64_t *phys, uint64_t *vram_bytes);
/* Reads a QEMU fw_cfg file into buf; returns the byte count, or a negative value when it does not exist. */
int fwcfg_read_file(const char *name, char *buf, size_t size);
