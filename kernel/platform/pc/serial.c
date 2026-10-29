#include "isa.h"
#include <asm/io.h>
#include <kernel/arch.h>
#include <kernel/drivers.h>
#include <kernel/platform.h>

#define COM1 0x3f8
#define VEC_COM1 0x24

static bool present;

static bool uart_init(uint16_t port, uint8_t loopback_byte) {
    outb(port + 1, 0x00); /* interrupts off */
    outb(port + 3, 0x80); /* DLAB on */
    outb(port + 0, 0x01); /* divisor 1 = 115200 baud */
    outb(port + 1, 0x00);
    outb(port + 3, 0x03); /* 8N1 */
    outb(port + 2, 0xc7); /* FIFO on, cleared, 14-byte threshold */
    outb(port + 4, 0x1e); /* loopback mode for the self-test */
    outb(port + 0, loopback_byte);
    if (inb(port + 0) != loopback_byte)
        return false;
    outb(port + 4, 0x0f); /* normal operation */
    return true;
}

bool serial_init(void) { return present = uart_init(COM1, 0xae); }

static void serial_putc(char c) {
    for (int spins = 0; spins < 100000 && !(inb(COM1 + 5) & 0x20); spins++)
        ;
    outb(COM1, (uint8_t)c);
}

void serial_write(const char *s, uint64_t len) {
    if (!present)
        return;
    for (uint64_t i = 0; i < len; i++) {
        if (s[i] == '\n')
            serial_putc('\r');
        serial_putc(s[i]);
    }
}

static void (*rx_callback)(char c);

static void com1_irq(struct regs *r) {
    (void)r;
    for (int n = 0; n < 64 && (inb(COM1 + 5) & 1); n++)
        rx_callback((char)inb(COM1));
    lapic_eoi();
}

void serial_rx_irq_init(void (*rx)(char c)) {
    rx_callback = rx;
    idt_set_handler(VEC_COM1, com1_irq);
    isa_irq_route(4, VEC_COM1);
    outb(COM1 + 1, 0x01); /* interrupt on received data */
}

/* ---- COM2: text from the host (clipboard sharing in VMs) ----
 * The VM launcher connects COM2 to a small program on the host that sends
 * the host's clipboard whenever it changes. the OS reads it from
 * /dev/hostclip; nothing here interprets it. */

#define COM2 0x2f8

static bool com2_present;

/* Whatever is already waiting (a clipboard sent while booting) is left
 * for the reader: draining it here in a tight loop is exactly the
 * pattern that, under some hypervisors (Windows' WHPX), keeps QEMU's
 * own thread from running and freezes its window. */
bool com2_init(void) { return com2_present = uart_init(COM2, 0x5a); }

/* Non-blocking: returns what has arrived (0 if nothing). Every port access
 * is a VM exit, so it reads only what is ready, a FIFO's worth at a time,
 * and never waits: the shell calls it every frame anyway. */
int64_t com2_read(void *buf, uint64_t len) {
    if (!com2_present)
        return 0;
    uint8_t *b = buf;
    uint64_t n = 0;
    while (n < len && n < 64 && (inb(COM2 + 5) & 1))
        b[n++] = inb(COM2);
    return (int64_t)n;
}
