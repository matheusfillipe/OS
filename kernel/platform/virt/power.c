#include <kernel/platform.h>

#define PSCI_SYSTEM_OFF   0x84000008
#define PSCI_SYSTEM_RESET 0x84000009

/* QEMU virt without EL2 or EL3 (HVF and KVM) takes PSCI calls through HVC. */
static void psci_call(uint32_t fn) {
    register uint64_t x0 __asm__("x0") = fn;
    __asm__ volatile("hvc #0" : "+r"(x0)::"x1", "x2", "x3", "memory");
}

/* PSCI has no exit status: a test harness tells success from failure by the
 * serial log. */
void platform_test_exit(uint8_t code) {
    (void)code;
    psci_call(PSCI_SYSTEM_OFF);
}

void platform_reboot(void) { psci_call(PSCI_SYSTEM_RESET); }

void platform_poweroff(void) { psci_call(PSCI_SYSTEM_OFF); }
