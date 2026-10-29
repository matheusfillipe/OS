#include <kernel/arch.h>
#include <kernel/drivers.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/platform.h>
#include <kernel/proc.h>
#include <kernel/vfs.h>
#include <linux/abi.h>

/* The system console as a Linux-style terminal: /dev/console, /dev/tty and
 * /dev/ttyS0. Output goes to the serial port and the boot framebuffer;
 * input comes from the serial port (and the keyboard driver, later) through
 * a small line discipline: canonical mode, echo, ^C ^\ ^Z ^D ^U, backspace. */

#define INBUF 4096

static struct {
    char buf[INBUF];
    uint64_t head, tail; /* produced / consumed */
    uint64_t line_end;   /* canonical mode: readable up to here */
    bool eof_pending;    /* ^D on an empty line: next read returns 0 */
    struct linux_termios termios;
    struct linux_winsize winsize;
    int fg_pgrp;
    struct waitq wq;
    spinlock_t lock;
} tty;

void console_write(const char *s, size_t len);

static void tty_output(const char *s, size_t len) {
    console_write(s, len); /* serial and screen, under the log's lock */
}

static bool tty_has_input(void *arg) {
    (void)arg;
    if (tty.termios.c_lflag & TC_ICANON)
        return tty.line_end != tty.tail || tty.eof_pending;
    return tty.head != tty.tail;
}

int proc_kill_pgrp(int pgid, int sig);

static void signal_fg(int sig) {
    if (tty.fg_pgrp > 0)
        proc_kill_pgrp(tty.fg_pgrp, sig);
}

static void echo_erase(void) {
    if (tty.termios.c_lflag & TC_ECHO)
        tty_output("\b \b", 3);
}

void tty_input(char c) {
    struct linux_termios *t = &tty.termios;
    if (c == '\r' && (t->c_iflag & TC_ICRNL))
        c = '\n';

    if (t->c_lflag & TC_ISIG) {
        int sig = c == (char)t->c_cc[VINTR]  ? SIGINT
                  : c == (char)t->c_cc[VQUIT] ? SIGQUIT
                  : c == (char)t->c_cc[VSUSP] ? SIGTSTP
                                              : 0;
        if (sig) {
            if (t->c_lflag & TC_ECHO) {
                char echo[4] = {'^', (char)(c + '@'), '\n', 0};
                tty_output(echo, 3);
            }
            uint64_t f = spin_lock_irqsave(&tty.lock);
            tty.head = tty.line_end; /* drop the partial line */
            spin_unlock_irqrestore(&tty.lock, f);
            signal_fg(sig);
            return;
        }
    }

    uint64_t f = spin_lock_irqsave(&tty.lock);
    if (!(t->c_lflag & TC_ICANON)) {
        if (tty.head - tty.tail < INBUF) {
            tty.buf[tty.head++ % INBUF] = c;
            tty.line_end = tty.head;
            if (t->c_lflag & TC_ECHO)
                tty_output(&c, 1);
        }
    } else if (c == (char)t->c_cc[VERASE] || c == '\b') {
        if (tty.head > tty.line_end) {
            tty.head--;
            echo_erase();
        }
    } else if (c == (char)t->c_cc[VKILL]) {
        while (tty.head > tty.line_end) {
            tty.head--;
            echo_erase();
        }
    } else if (c == (char)t->c_cc[VEOF]) {
        if (tty.head == tty.line_end)
            tty.eof_pending = true;
        tty.line_end = tty.head;
    } else if (tty.head - tty.tail < INBUF - 1) {
        tty.buf[tty.head++ % INBUF] = c;
        if (t->c_lflag & TC_ECHO)
            tty_output(&c, 1);
        if (c == '\n')
            tty.line_end = tty.head;
    }
    spin_unlock_irqrestore(&tty.lock, f);
    waitq_wake_all(&tty.wq);
}

static int64_t tty_read(struct vnode *v, struct file *f, void *buf, size_t len, uint64_t off) {
    (void)v;
    (void)off;
    for (;;) {
        uint64_t fl = spin_lock_irqsave(&tty.lock);
        bool canon = tty.termios.c_lflag & TC_ICANON;
        uint64_t limit = canon ? tty.line_end : tty.head;
        if (limit != tty.tail) {
            size_t n = 0;
            while (n < len && tty.tail < limit) {
                char c = tty.buf[tty.tail++ % INBUF];
                ((char *)buf)[n++] = c;
                if (canon && c == '\n')
                    break;
            }
            spin_unlock_irqrestore(&tty.lock, fl);
            return (int64_t)n;
        }
        if (canon && tty.eof_pending) {
            tty.eof_pending = false;
            spin_unlock_irqrestore(&tty.lock, fl);
            return 0;
        }
        spin_unlock_irqrestore(&tty.lock, fl);
        if (f && (f->flags & O_NONBLOCK))
            return -EAGAIN;
        if (waitq_wait_cond(&tty.wq, tty_has_input, NULL) < 0)
            return -EINTR;
    }
}

static spinlock_t write_lock = SPINLOCK_INIT;

static int64_t tty_write(struct vnode *v, struct file *f, const void *buf, size_t len, uint64_t off) {
    (void)v;
    (void)f;
    (void)off;
    /* One writer at a time, so lines from different programs don't mix. */
    uint64_t fl = spin_lock_irqsave(&write_lock);
    tty_output(buf, len);
    spin_unlock_irqrestore(&write_lock, fl);
    return (int64_t)len;
}

static int64_t tty_ioctl(struct vnode *v, struct file *f, uint64_t req, uint64_t arg) {
    (void)v;
    (void)f;
    switch (req) {
    case TCGETS:
        return copy_to_user(arg, &tty.termios, sizeof tty.termios);
    case TCGETS2: {
        struct linux_termios2 t2 = {tty.termios, 38400, 38400};
        return copy_to_user(arg, &t2, sizeof t2);
    }
    case TCSETS2:
    case TCSETSW2:
    case TCSETSF2:
    case TCSETS:
    case TCSETSW:
    case TCSETSF: {
        struct linux_termios t;
        int r = copy_from_user(&t, arg, sizeof t); /* termios2 starts with the same fields */
        if (req == TCSETSF2)
            req = TCSETSF;
        if (r == 0) {
            uint64_t fl = spin_lock_irqsave(&tty.lock);
            bool was_canon = tty.termios.c_lflag & TC_ICANON;
            tty.termios = t;
            if (was_canon && !(t.c_lflag & TC_ICANON))
                tty.line_end = tty.head; /* pending partial line becomes readable */
            if (req == TCSETSF)
                tty.tail = tty.line_end = tty.head;
            spin_unlock_irqrestore(&tty.lock, fl);
        }
        return r;
    }
    case TIOCGWINSZ:
        return copy_to_user(arg, &tty.winsize, sizeof tty.winsize);
    case TIOCSWINSZ:
        return copy_from_user(&tty.winsize, arg, sizeof tty.winsize);
    case TIOCGPGRP:
        return copy_to_user(arg, &tty.fg_pgrp, sizeof tty.fg_pgrp);
    case TIOCSPGRP:
        return copy_from_user(&tty.fg_pgrp, arg, sizeof tty.fg_pgrp);
    case TIOCSCTTY:
        return 0;
    case FIONREAD: {
        int n = (int)(tty.line_end - tty.tail);
        return copy_to_user(arg, &n, sizeof n);
    }
    }
    return -ENOTTY;
}

static int tty_poll(struct vnode *v, struct file *f, struct waitq **wq) {
    (void)v;
    (void)f;
    if (wq)
        *wq = &tty.wq;
    return POLLOUT | (tty_has_input(NULL) ? POLLIN : 0);
}

static const struct vnode_ops tty_ops = {
    .read = tty_read, .write = tty_write, .ioctl = tty_ioctl, .poll = tty_poll,
};

/* /dev/tty is the caller's controlling terminal: the console, or a pty. */
static int devtty_open(struct vnode *v, struct file *f) {
    (void)v;
    struct process *cur = proc_current();
    if (cur->ctty < 0)
        return -ENXIO;
    if (cur->ctty == 0)
        return 0;
    struct vnode *s = pty_slave_vnode(cur->ctty - 1);
    if (!s)
        return -ENXIO;
    vnode_put(f->vnode);
    f->vnode = s;
    return s->ops->open ? s->ops->open(s, f) : 0;
}

static const struct vnode_ops devtty_ops = {
    .read = tty_read, .write = tty_write, .ioctl = tty_ioctl, .poll = tty_poll, .open = devtty_open,
};

static void serial_rx(char c) {
    if (c == 0x7f)
        c = '\b';
    tty_input(c);
}

void tty_init(void) {
    waitq_init(&tty.wq);
    struct linux_termios *t = &tty.termios;
    t->c_iflag = TC_ICRNL;
    t->c_oflag = TC_OPOST | TC_ONLCR;
    t->c_cflag = 0x00bf; /* B38400 | CS8 | CREAD */
    t->c_lflag = TC_ISIG | TC_ICANON | TC_ECHO | TC_ECHOE | TC_ECHOK | TC_ECHOCTL | TC_IEXTEN;
    t->c_cc[VINTR] = 3;    /* ^C */
    t->c_cc[VQUIT] = 0x1c; /* ^\ */
    t->c_cc[VERASE] = 0x7f;
    t->c_cc[VKILL] = 0x15; /* ^U */
    t->c_cc[VEOF] = 4;     /* ^D */
    t->c_cc[VMIN] = 1;
    t->c_cc[VSUSP] = 0x1a; /* ^Z */
    tty.winsize.ws_row = 24;
    tty.winsize.ws_col = 80;

    struct vnode *v = vnode_alloc(VN_CHR, 0620, &tty_ops);
    KASSERT(v);
    v->rdev = (5 << 8) | 1; /* major 5, minor 1: /dev/console */
    vfs_create_node("/dev/console", v);
    vfs_create_node("/dev/ttyS0", v);
    vnode_put(v);
    v = vnode_alloc(VN_CHR, 0666, &devtty_ops);
    KASSERT(v);
    v->rdev = (5 << 8) | 0;
    vfs_create_node("/dev/tty", v);
    vnode_put(v);

    platform_console_rx_init(serial_rx);
}

/* ---- the simple character devices ---- */

static int64_t null_read(struct vnode *v, struct file *f, void *buf, size_t len, uint64_t off) {
    (void)v, (void)f, (void)buf, (void)len, (void)off;
    return 0;
}
static int64_t sink_write(struct vnode *v, struct file *f, const void *buf, size_t len, uint64_t off) {
    (void)v, (void)f, (void)buf, (void)off;
    return (int64_t)len;
}
static int64_t zero_read(struct vnode *v, struct file *f, void *buf, size_t len, uint64_t off) {
    (void)v, (void)f, (void)off;
    memset(buf, 0, len);
    return (int64_t)len;
}

/* xorshift64* seeded from the arch cycle counter (and a hardware RNG when present). Not a CSPRNG
 * yet: the entropy pool arrives with the crypto work for TLS. */
static uint64_t rng_state;
static uint64_t rng_next(void) {
    uint64_t x = rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}
void random_bytes(void *buf, size_t len);
void random_bytes(void *buf, size_t len) {
    uint8_t *b = buf;
    for (size_t i = 0; i < len; i++) {
        if (i % 8 == 0) {
            rng_state ^= arch_counter();
        }
        b[i] = (uint8_t)(rng_next() >> 32);
    }
}
static int64_t random_read(struct vnode *v, struct file *f, void *buf, size_t len, uint64_t off) {
    (void)v, (void)f, (void)off;
    random_bytes(buf, len);
    return (int64_t)len;
}

static int64_t hostclip_read(struct vnode *v, struct file *f, void *buf, size_t len, uint64_t off) {
    (void)v, (void)f, (void)off;
    return platform_hostlink_read(buf, len);
}

static const struct vnode_ops null_ops = {.read = null_read, .write = sink_write};
static const struct vnode_ops hostclip_ops = {.read = hostclip_read, .write = sink_write};
static const struct vnode_ops zero_ops = {.read = zero_read, .write = sink_write};
static const struct vnode_ops random_ops = {.read = random_read, .write = sink_write};
/* /dev/full: reads zeros, and every write finds the disk full. */
static int64_t full_write(struct vnode *v, struct file *f, const void *buf, size_t len, uint64_t off) {
    (void)v, (void)f, (void)buf, (void)len, (void)off;
    return -ENOSPC;
}
static const struct vnode_ops full_ops = {.read = zero_read, .write = full_write};

static void add_dev(const char *path, const struct vnode_ops *ops, uint32_t major, uint32_t minor) {
    struct vnode *v = vnode_alloc(VN_CHR, 0666, ops);
    KASSERT(v);
    v->rdev = (major << 8) | minor;
    vfs_create_node(path, v);
    vnode_put(v);
}

void devfs_init(void) {
    vfs_mkdir_p("/dev", 0755);
    rng_state = arch_counter() | 1;
    add_dev("/dev/null", &null_ops, 1, 3);
    add_dev("/dev/zero", &zero_ops, 1, 5);
    add_dev("/dev/full", &full_ops, 1, 7);
    add_dev("/dev/random", &random_ops, 1, 8);
    add_dev("/dev/urandom", &random_ops, 1, 9);
    if (platform_hostlink_init())
        add_dev("/dev/hostclip", &hostclip_ops, 4, 65);
    tty_init();
    pty_init();
}
