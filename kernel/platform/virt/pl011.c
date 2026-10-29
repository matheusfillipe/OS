#include "virt.h"
#include <kernel/arch.h>
#include <kernel/boot.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/platform.h>

#define UART_DR     0x00
#define UART_FR     0x18
#define UART_CR     0x30
#define UART_IMSC   0x38
#define UART_ICR    0x44
#define UART_PERIPHID0 0xfe0
#define FR_RXFE     (1u << 4)
#define FR_TXFF     (1u << 5)
#define CR_UARTEN   (1u << 0)
#define CR_TXE      (1u << 8)
#define CR_RXE      (1u << 9)
#define INT_RX      (1u << 4)
#define INT_RT      (1u << 6)

static volatile uint32_t *uart;
static uint64_t early_tables[3][512] __attribute__((aligned(4096)));

/* Limine's direct map leaves device memory out and TTBR0 is ours from base
 * revision 1 on, so until platform_console_remap we identity-map the UART's
 * 2 MiB there, through a MAIR entry Limine does not use. */
static void map_uart_early(void) {
    uint64_t va = VIRT_UART;
    early_tables[0][(va >> 39) & 511] = pte_table(kernel_virt_to_phys(early_tables[1]), false);
    early_tables[1][(va >> 30) & 511] = pte_table(kernel_virt_to_phys(early_tables[2]), false);
    early_tables[2][(va >> 21) & 511] = pte_make_huge(va, PG_WRITE | PG_UNCACHED);
    uint64_t mair;
    __asm__ volatile("mrs %0, mair_el1" : "=r"(mair));
    mair = (mair & ~(0xffULL << (8 * MAIR_DEVICE))) | (uint64_t)MAIR_DEVICE_ATTR << (8 * MAIR_DEVICE);
    __asm__ volatile("msr mair_el1, %0; isb" ::"r"(mair));
    __asm__ volatile("dsb ishst; msr ttbr0_el1, %0; isb; tlbi vmalle1; dsb ish; isb" ::"r"(
                         kernel_virt_to_phys(early_tables[0]))
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

void platform_console_remap(void) {
    if (!uart)
        return;
    vmm_map_mmio(VIRT_UART, 0x1000, true);
    uart = phys_to_virt(VIRT_UART);
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

static void (*rx_callback)(char c);

static void uart_irq(struct regs *r) {
    (void)r;
    for (int n = 0; n < 64 && !(uart[UART_FR / 4] & FR_RXFE); n++)
        rx_callback((char)uart[UART_DR / 4]);
    uart[UART_ICR / 4] = INT_RX | INT_RT;
    irq_eoi();
}

void platform_console_rx_init(void (*rx)(char c)) {
    if (!uart)
        return;
    rx_callback = rx;
    irq_set_handler(VIRT_UART_IRQ, uart_irq);
    uart[UART_IMSC / 4] = INT_RX | INT_RT;
}

bool platform_hostlink_init(void) { return false; }

int64_t platform_hostlink_read(void *buf, uint64_t len) {
    (void)buf, (void)len;
    return 0;
}
