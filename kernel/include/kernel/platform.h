#pragma once
#include <stdint.h>

void serial_rx_irq_init(void (*rx)(char c));
void i8042_init(void);
int64_t platform_rtc_read(void);
void platform_reboot(void);
void platform_poweroff(void);
void platform_test_exit(uint8_t code);
