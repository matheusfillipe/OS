#include <kernel/input.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/platform.h>
#include <kernel/proc.h>
#include <kernel/vfs.h>

/* Keyboard and mouse as Linux evdev devices (/dev/input/event0 keyboard,
 * /dev/input/event1 pointer), so programs read struct input_event exactly
 * as on Linux. The platform's input sources feed them through evdev_push. */

struct input_event {
    int64_t sec, usec;
    uint16_t type, code;
    int32_t value;
};

#define QLEN 512

struct evdev {
    struct input_event q[QLEN];
    uint32_t head, tail;
    int opens;
    struct waitq wq;
    spinlock_t lock;
};

struct evdev input_kbd, input_mouse;

void evdev_push(struct evdev *d, uint16_t type, uint16_t code, int32_t value) {
    uint64_t ns = time_monotonic_ns();
    uint64_t f = spin_lock_irqsave(&d->lock);
    if (d->head - d->tail < QLEN) {
        struct input_event *e = &d->q[d->head++ % QLEN];
        e->sec = (int64_t)(ns / 1000000000ULL);
        e->usec = (int64_t)(ns % 1000000000ULL / 1000);
        e->type = type;
        e->code = code;
        e->value = value;
    }
    spin_unlock_irqrestore(&d->lock, f);
}

void evdev_sync(struct evdev *d) {
    evdev_push(d, EV_SYN, 0, 0);
    waitq_wake_all(&d->wq);
}

bool evdev_is_open(struct evdev *d) { return d->opens > 0; }

static bool has_events(void *arg) {
    struct evdev *d = arg;
    return d->head != d->tail;
}

static int64_t ev_read(struct vnode *v, struct file *f, void *buf, size_t len, uint64_t off) {
    (void)off;
    struct evdev *d = v->data;
    if (len < sizeof(struct input_event))
        return -EINVAL;
    for (;;) {
        uint64_t fl = spin_lock_irqsave(&d->lock);
        size_t n = 0;
        while (d->tail != d->head && (n + 1) * sizeof(struct input_event) <= len) {
            ((struct input_event *)buf)[n++] = d->q[d->tail++ % QLEN];
        }
        spin_unlock_irqrestore(&d->lock, fl);
        if (n)
            return (int64_t)(n * sizeof(struct input_event));
        if (f && (f->flags & O_NONBLOCK))
            return -EAGAIN;
        if (waitq_wait_cond(&d->wq, has_events, d) < 0)
            return -EINTR;
    }
}

static int ev_poll(struct vnode *v, struct file *f, struct waitq **wq) {
    (void)f;
    struct evdev *d = v->data;
    if (wq)
        *wq = &d->wq;
    return has_events(d) ? POLLIN : 0;
}

static int ev_open(struct vnode *v, struct file *f) {
    (void)f;
    struct evdev *d = v->data;
    __atomic_add_fetch(&d->opens, 1, __ATOMIC_RELAXED);
    return 0;
}

static void ev_close(struct vnode *v, struct file *f) {
    (void)f;
    struct evdev *d = v->data;
    __atomic_sub_fetch(&d->opens, 1, __ATOMIC_RELAXED);
}

static const struct vnode_ops ev_ops = {.read = ev_read, .poll = ev_poll, .open = ev_open, .close = ev_close};

static void add_dev(const char *path, struct evdev *d) {
    waitq_init(&d->wq);
    struct vnode *v = vnode_alloc(VN_CHR, 0660, &ev_ops);
    KASSERT(v);
    v->data = d;
    v->rdev = (13 << 8) | (d == &input_kbd ? 64 : 65);
    vfs_create_node(path, v);
    vnode_put(v);
}

void input_init(void) {
    vfs_mkdir_p("/dev/input", 0755);
    add_dev("/dev/input/event0", &input_kbd);
    add_dev("/dev/input/event1", &input_mouse);
    platform_input_init();
}
