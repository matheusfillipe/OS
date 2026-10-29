#pragma once
#include <stdbool.h>
#include <stdint.h>

#define EV_SYN 0
#define EV_KEY 1
#define EV_REL 2
#define EV_ABS 3
#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111
#define BTN_MIDDLE 0x112

struct evdev;
extern struct evdev input_kbd, input_mouse;

void evdev_push(struct evdev *d, uint16_t type, uint16_t code, int32_t value);
void evdev_sync(struct evdev *d);
bool evdev_is_open(struct evdev *d);
