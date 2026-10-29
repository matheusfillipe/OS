#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Text console drawn on the bootloader framebuffer. Only used for boot and
 * emergencies; the real desktop is drawn by the compositor. */
void fbcon_init(void);
void fbcon_write(const char *s, uint64_t len);
void fbcon_suspend(bool on); /* stop drawing once a program owns the screen */
void fbdev_init(void);
void input_init(void);
