#include "virt.h"
#include <kernel/boot.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/platform.h>

#define UART_DR     0x00
#define UART_FR     0x18
#define UART_CR     0x30
#define UART_PERIPHID0 0xfe0
#define FR_TXFF     (1u << 5)
#define CR_UARTEN   (1u << 0)
#define CR_TXE      (1u << 8)
#define CR_RXE      (1u << 9)

static volatile uint32_t *uart;
static uint64_t early_tables[3][512] __attribute__((aligned(4096)));

static uint64_t kernel_phys(const void *p) {
    struct limine_executable_address_response *ka = boot_kaddr_request.response;
    return (uint64_t)p - ka->virtual_base + ka->physical_base;
}

/* Limine's direct map leaves device memory out and TTBR0 is ours from base
 * revision 1 on, so we identity-map the UART's 2 MiB there, through a MAIR
 * entry Limine does not use. */
static void map_uart_early(void) {
    uint64_t va = VIRT_UART;
    early_tables[0][(va >> 39) & 511] = pte_table(kernel_phys(early_tables[1]), false);
    early_tables[1][(va >> 30) & 511] = pte_table(kernel_phys(early_tables[2]), false);
    early_tables[2][(va >> 21) & 511] = pte_make_huge(va, PG_WRITE | PG_UNCACHED);
    uint64_t mair;
    __asm__ volatile("mrs %0, mair_el1" : "=r"(mair));
    mair = (mair & ~(0xffULL << (8 * MAIR_DEVICE))) | (uint64_t)MAIR_DEVICE_ATTR << (8 * MAIR_DEVICE);
    __asm__ volatile("msr mair_el1, %0; isb" ::"r"(mair));
    __asm__ volatile("dsb ishst; msr ttbr0_el1, %0; isb; tlbi vmalle1; dsb ish; isb" ::"r"(
                         kernel_phys(early_tables[0]))
                     : "memory");
}

bool platform_console_init(void) {
    if (!boot_kaddr_request.response)
        return false;
    map_uart_early();
    volatile uint32_t *r = (volatile uint32_t *)VIRT_UART;
    if ((r[UART_PERIPHID0 / 4] & 0xff) != 0x11)
        return false;
    r[UART_CR / 4] = CR_UARTEN | CR_TXE | CR_RXE;
    uart = r;
    return true;
}

static void uart_putc(char c) {
    for (int spins = 0; spins < 100000 && (uart[UART_FR / 4] & FR_TXFF); spins++)
        ;
    uart[UART_DR / 4] = (uint8_t)c;
}

void platform_console_write(const char *s, uint64_t len) {
    if (!uart)
        return;
    for (uint64_t i = 0; i < len; i++) {
        if (s[i] == '\n')
            uart_putc('\r');
        uart_putc(s[i]);
    }
}

void platform_console_rx_init(void (*rx)(char c)) {
    (void)rx;
    panic("virt: PL011 receive interrupts need the GIC, which is not implemented yet");
}

bool platform_hostlink_init(void) { return false; }

int64_t platform_hostlink_read(void *buf, uint64_t len) {
    (void)buf, (void)len;
    return 0;
}
