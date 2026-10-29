#include "isa.h"
#include <asm/io.h>
#include <kernel/arch.h>
#include <kernel/input.h>
#include <kernel/lib.h>
#include <kernel/platform.h>
#include <kernel/vfs.h>

/* Keyboard and mouse sources: the i8042 PS/2 controller, plus the VMware/QEMU
 * "vmmouse" backdoor for absolute pointer positions (a mouse that follows
 * the host cursor without grabbing). While nobody has the keyboard open,
 * keys also go to the console terminal. */

/* ---- PS/2 keyboard: scan code set 1 (translated) -> Linux key codes ---- */

/* For set 1 make codes 0x01-0x58 the Linux KEY_* number equals the code. */
static uint16_t e0_key(uint8_t sc) {
    switch (sc) {
    case 0x1c: return 96;  /* KP enter */
    case 0x1d: return 97;  /* right ctrl */
    case 0x35: return 98;  /* KP slash */
    case 0x37: return 99;  /* print screen (E0 2A E0 37; the E0 2A half is a fake shift, ignored) */
    case 0x38: return 100; /* right alt */
    case 0x47: return 102; /* home */
    case 0x48: return 103; /* up */
    case 0x49: return 104; /* page up */
    case 0x4b: return 105; /* left */
    case 0x4d: return 106; /* right */
    case 0x4f: return 107; /* end */
    case 0x50: return 108; /* down */
    case 0x51: return 109; /* page down */
    case 0x52: return 110; /* insert */
    case 0x53: return 111; /* delete */
    case 0x5b: return 125; /* left meta */
    case 0x5c: return 126; /* right meta */
    case 0x5d: return 127; /* menu */
    }
    return 0;
}

/* US layout for feeding the console when no program reads the keyboard. */
static const char plain[0x3a] = {0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
                                 '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\r', 0,
                                 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,   '\\', 'z',
                                 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,   '*', 0,   ' '};
static const char shifted[0x3a] = {0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
                                   '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\r', 0,
                                   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,   '|', 'Z',
                                   'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,   '*', 0,   ' '};
static bool shift, ctrl, e0;

static void kbd_byte(uint8_t sc) {
    if (sc == 0xe0) {
        e0 = true;
        return;
    }
    bool release = sc & 0x80;
    uint8_t code = sc & 0x7f;
    uint16_t key = e0 ? e0_key(code) : code;
    bool was_e0 = e0;
    e0 = false;
    if (!key)
        return;
    if (key == 42 || key == 54)
        shift = !release;
    if (key == 29 || key == 97)
        ctrl = !release;

    if (evdev_is_open(&input_kbd)) {
        evdev_push(&input_kbd, EV_KEY, key, release ? 0 : 1);
        evdev_sync(&input_kbd);
        return;
    }
    if (release || was_e0 || code >= sizeof plain)
        return;
    char c = shift ? shifted[code] : plain[code];
    if (ctrl && c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 1);
    if (c)
        tty_input(c);
}

/* ---- vmmouse (VMware backdoor; also implemented by QEMU) ---- */

#define VMW_MAGIC 0x564D5868
#define VMW_PORT 0x5658

static void backdoor(uint32_t cmd, uint32_t arg, uint32_t out[4]) {
    uint32_t a = VMW_MAGIC, b = arg, c = cmd, d = VMW_PORT;
    __asm__ volatile("inl %%dx, %%eax" : "+a"(a), "+b"(b), "+c"(c), "+d"(d));
    out[0] = a, out[1] = b, out[2] = c, out[3] = d;
}

static bool vmmouse;
static uint32_t last_buttons;

static bool vmmouse_init(void) {
    uint32_t r[4];
    backdoor(10 /* GETVERSION */, 0, r);
    if (r[1] != VMW_MAGIC)
        return false;
    backdoor(41 /* ABSPOINTER_COMMAND */, 0x45414552 /* ENABLE */, r);
    backdoor(40 /* ABSPOINTER_STATUS */, 0, r);
    if ((r[0] & 0x0000ffff) == 0)
        return false;
    backdoor(39 /* ABSPOINTER_DATA */, 1, r);
    if (r[0] != 0x3442554a) /* "JUB4": version ack */
        return false;
    backdoor(41, 0x53424152 /* REQUEST_ABSOLUTE */, r);
    return true;
}

static void vmmouse_poll(void) {
    for (int guard = 0; guard < 32; guard++) {
        uint32_t r[4];
        backdoor(40, 0, r);
        uint32_t queued = r[0] & 0xffff;
        if (r[0] == 0xffff0000 || queued < 4)
            return;
        backdoor(39, 4, r);
        uint32_t buttons = r[0] & 0xffff;
        evdev_push(&input_mouse, EV_ABS, 0, (int32_t)r[1]);
        evdev_push(&input_mouse, EV_ABS, 1, (int32_t)r[2]);
        int32_t wheel = (int8_t)(r[3] & 0xff);
        if (wheel)
            evdev_push(&input_mouse, EV_REL, 8, -wheel);
        static const struct { uint32_t bit; uint16_t code; } map[] = {{0x20, BTN_LEFT}, {0x10, BTN_RIGHT}, {0x08, BTN_MIDDLE}};
        for (int i = 0; i < 3; i++)
            if ((buttons ^ last_buttons) & map[i].bit)
                evdev_push(&input_mouse, EV_KEY, map[i].code, (buttons & map[i].bit) ? 1 : 0);
        last_buttons = buttons;
        evdev_sync(&input_mouse);
    }
}

/* ---- PS/2 mouse (relative) ---- */

static uint8_t packet[3];
static int packet_len;

static void mouse_byte(uint8_t b) {
    if (vmmouse) {
        vmmouse_poll();
        return;
    }
    if (packet_len == 0 && !(b & 0x08))
        return; /* resync: first byte always has bit 3 set */
    packet[packet_len++] = b;
    if (packet_len < 3)
        return;
    packet_len = 0;
    int dx = packet[1] - ((packet[0] << 4) & 0x100);
    int dy = packet[2] - ((packet[0] << 3) & 0x100);
    if (dx)
        evdev_push(&input_mouse, EV_REL, 0, dx);
    if (dy)
        evdev_push(&input_mouse, EV_REL, 1, -dy);
    uint32_t buttons = packet[0] & 7;
    static const uint16_t codes[3] = {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE};
    for (int i = 0; i < 3; i++)
        if ((buttons ^ last_buttons) & (1u << i))
            evdev_push(&input_mouse, EV_KEY, codes[i], (int32_t)((buttons >> i) & 1));
    last_buttons = buttons;
    evdev_sync(&input_mouse);
}

/* ---- i8042 controller ---- */

#define VEC_KBD 0x25
#define VEC_MOUSE 0x26

static void i8042_wait_write(void) {
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++)
        ;
}

static void i8042_wait_read(void) {
    for (int i = 0; i < 100000 && !(inb(0x64) & 1); i++)
        ;
}

static void i8042_cmd(uint8_t c) {
    i8042_wait_write();
    outb(0x64, c);
}

static void mouse_write(uint8_t v) {
    i8042_cmd(0xd4);
    i8042_wait_write();
    outb(0x60, v);
    i8042_wait_read();
    (void)inb(0x60); /* ACK */
}

static void i8042_irq(struct regs *r) {
    (void)r;
    for (int n = 0; n < 32 && (inb(0x64) & 1); n++) {
        uint8_t status = inb(0x64);
        uint8_t data = inb(0x60);
        if (status & 0x20)
            mouse_byte(data);
        else
            kbd_byte(data);
    }
    lapic_eoi();
}

void i8042_init(void) {
    /* Quiet both ports, flush, enable the aux port and both interrupts,
     * keep translation. The flush is bounded: under Windows' hypervisor a
     * mouse moving over the window keeps the queue filling, and an endless
     * port loop there freezes QEMU itself. */
    i8042_cmd(0xad);
    i8042_cmd(0xa7);
    for (int i = 0; i < 64 && (inb(0x64) & 1); i++)
        (void)inb(0x60);
    i8042_cmd(0xae);
    i8042_cmd(0xa8);
    i8042_cmd(0x20);
    i8042_wait_read();
    uint8_t cfg = inb(0x60);
    cfg |= 0x03;  /* IRQ1 + IRQ12 */
    cfg &= ~0x30; /* clocks on for both ports */
    i8042_cmd(0x60);
    i8042_wait_write();
    outb(0x60, cfg);
    /* The absolute (vmmouse) pointer first: until it is on, QEMU's window
     * treats a host mouse moving over it as a relative one, and under
     * Windows' hypervisor that can starve this CPU for as long as it moves. */
    vmmouse = vmmouse_init();
    mouse_write(0xf6); /* defaults */
    mouse_write(0xf4); /* streaming on */

    idt_set_handler(VEC_KBD, i8042_irq);
    idt_set_handler(VEC_MOUSE, i8042_irq);
    isa_irq_route(1, VEC_KBD);
    isa_irq_route(12, VEC_MOUSE);
    kprintf("input: PS/2 keyboard, %s mouse\n", vmmouse ? "absolute (vmmouse)" : "PS/2");
}
