#pragma once

static inline void cli(void) { __asm__ volatile("cli"); }
static inline void sti(void) { __asm__ volatile("sti"); }
static inline void hlt(void) { __asm__ volatile("hlt"); }

/* Writing here makes QEMU exit when started with -device isa-debug-exit;
 * on real hardware and in `make run` the port is unused and this is a no-op. */
#define QEMU_DEBUG_EXIT_PORT 0xf4
