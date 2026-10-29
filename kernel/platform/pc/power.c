#include <asm/io.h>
#include <kernel/platform.h>

/* Writing here makes QEMU exit when started with -device isa-debug-exit;
 * on real hardware and in `make run` the port is unused and this is a no-op. */
#define QEMU_DEBUG_EXIT_PORT 0xf4

void platform_test_exit(uint8_t code) { outb(QEMU_DEBUG_EXIT_PORT, code); }

void platform_reboot(void) { outb(0x64, 0xfe); /* keyboard-controller reset */ }

void platform_poweroff(void) { outw(0x604, 0x2000); /* QEMU q35 PM1a_CNT: SLP_EN with S5 */ }
