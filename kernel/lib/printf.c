#include <kernel/drivers.h>
#include <kernel/lib.h>
#include <kernel/platform.h>
#include <kernel/spinlock.h>

/* Minimal printf: %d %i %u %x %X %p %s %c %%, with optional '0' flag, width,
 * and l/ll/z length modifiers. Enough for kernel logging. */

struct out {
    char *buf;
    size_t size, len;
};

static void put(struct out *o, char c) {
    if (o->len + 1 < o->size)
        o->buf[o->len] = c;
    o->len++;
}

static void put_num(struct out *o, uint64_t v, unsigned base, bool upper, bool neg, int width, char pad) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    int n = 0;
    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v);
    int total = n + (neg ? 1 : 0);
    if (neg && pad == '0')
        put(o, '-');
    for (; width > total; width--)
        put(o, pad);
    if (neg && pad != '0')
        put(o, '-');
    while (n)
        put(o, tmp[--n]);
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    struct out o = {buf, size, 0};
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            put(&o, *fmt);
            continue;
        }
        fmt++;
        char pad = ' ';
        int width = 0, lng = 0;
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        int prec = -1;
        if (*fmt == '.') {
            prec = 0;
            fmt++;
            while (*fmt >= '0' && *fmt <= '9')
                prec = prec * 10 + (*fmt++ - '0');
        }
        while (*fmt == 'l' || *fmt == 'z') {
            lng++;
            fmt++;
        }
        switch (*fmt) {
        case 'd':
        case 'i': {
            int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
            put_num(&o, v < 0 ? -(uint64_t)v : (uint64_t)v, 10, false, v < 0, width, pad);
            break;
        }
        case 'u':
            put_num(&o, lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 10, false, false, width, pad);
            break;
        case 'x':
        case 'X':
            put_num(&o, lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 16, *fmt == 'X', false, width, pad);
            break;
        case 'p':
            put(&o, '0');
            put(&o, 'x');
            put_num(&o, (uint64_t)va_arg(ap, void *), 16, false, false, 16, '0');
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            size_t l = 0;
            while (s[l] && (prec < 0 || l < (size_t)prec))
                l++;
            for (; width > (int)l; width--)
                put(&o, ' ');
            for (size_t i = 0; i < l; i++)
                put(&o, s[i]);
            break;
        }
        case 'c':
            put(&o, (char)va_arg(ap, int));
            break;
        case '%':
            put(&o, '%');
            break;
        case '\0':
            fmt--;
            break;
        default:
            put(&o, '%');
            put(&o, *fmt);
        }
    }
    if (size)
        buf[o.len < size ? o.len : size - 1] = '\0';
    return (int)o.len;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

static void log_write(const char *s, size_t len) {
    serial_write(s, len);
    fbcon_write(s, len);
}

static spinlock_t log_lock = SPINLOCK_INIT;

/* Programs' output to the console goes through the same lock as the
 * kernel's messages, so the two never interleave mid-line in the log. */
void console_write(const char *s, size_t len);
void console_write(const char *s, size_t len) {
    uint64_t f = spin_lock_irqsave(&log_lock);
    log_write(s, len);
    spin_unlock_irqrestore(&log_lock, f);
}

void kprintf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    uint64_t f = spin_lock_irqsave(&log_lock);
    log_write(buf, n < (int)sizeof buf ? (size_t)n : sizeof buf - 1);
    spin_unlock_irqrestore(&log_lock, f);
}

void halt_forever(void) {
    for (;;) {
        irq_disable();
        halt();
    }
}

void panic(const char *fmt, ...) {
    irq_disable();
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log_write("\n*** KERNEL PANIC: ", 19);
    log_write(buf, n < (int)sizeof buf ? (size_t)n : sizeof buf - 1);
    log_write("\n", 1);
    /* where it came from: the frame-pointer chain (addr2line -e kernel.elf) */
    uint64_t *fp = __builtin_frame_address(0);
    for (int i = 0; i < 16 && fp && (uint64_t)fp >= 0xffff800000000000ULL && ((uint64_t)fp & 7) == 0; i++) {
        char line[40];
        int l = ksnprintf(line, sizeof line, "  at %lx\n", fp[1]);
        log_write(line, (size_t)l);
        fp = (uint64_t *)fp[0];
    }
    platform_test_exit(0x01); /* QEMU exit code 3 = failure */
    halt_forever();
}
