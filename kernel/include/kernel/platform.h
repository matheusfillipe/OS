#pragma once
#include <stddef.h>
#include <stdint.h>

void serial_rx_irq_init(void (*rx)(char c));
void i8042_init(void);
int64_t platform_rtc_read(void);
void platform_reboot(void);
void platform_poweroff(void);
void platform_test_exit(uint8_t code);
/* Reads a QEMU fw_cfg file into buf; returns the byte count, or a negative value when it does not exist. */
int fwcfg_read_file(const char *name, char *buf, size_t size);
