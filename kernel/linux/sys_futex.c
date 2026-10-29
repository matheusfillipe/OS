#include <kernel/lib.h>
#include <kernel/proc.h>
#include <linux/syscall.h>

/* futex: the kernel half of every user-space lock, condition variable,
 * pthread_join and once-flag in glibc and musl.
 *
 * Waiters sit in a hash table of queues keyed by (mm, user address). Threads
 * of one process share the mm, so that names exactly the threads that can
 * see the same futex word. Shared (non-private) futexes use the same key:
 * without memory shared between address spaces, only CLONE_VM relatives
 * could share a futex word, and they share the mm too. */

struct futex_bucket;

struct futex_waiter {
    struct thread *t;
    struct mm *mm;
    uint64_t uaddr;
    uint32_t bitset;
    bool woken; /* set by the waker, under the bucket lock */
    struct futex_bucket *bucket; /* the queue we are on; requeue moves us */
    struct futex_waiter *next;
};

struct futex_bucket {
    spinlock_t lock;
    struct futex_waiter *head;
};

#define FUTEX_BUCKETS 256
static struct futex_bucket buckets[FUTEX_BUCKETS];

#define NS_PER_TICK (1000000000ULL / HZ)

static struct futex_bucket *bucket_for(struct mm *mm, uint64_t uaddr) {
    uint64_t h = ((uaddr >> 2) ^ ((uint64_t)mm >> 4)) * 0x9e3779b97f4a7c15ULL;
    return &buckets[h >> 56]; /* top 8 bits: FUTEX_BUCKETS == 256 */
}

/* FIFO, so a steady stream of waiters cannot starve the oldest one. */
static void enqueue(struct futex_bucket *b, struct futex_waiter *w) {
    w->next = NULL;
    __atomic_store_n(&w->bucket, b, __ATOMIC_RELEASE);
    struct futex_waiter **pp = &b->head;
    while (*pp)
        pp = &(*pp)->next;
    *pp = w;
}

static void dequeue(struct futex_bucket *b, struct futex_waiter *w) {
    for (struct futex_waiter **pp = &b->head; *pp; pp = &(*pp)->next)
        if (*pp == w) {
            *pp = w->next;
            return;
        }
}

/* Locks the queue `w` is on. A requeue may move it between our read of
 * w->bucket and the lock, so check again once we hold it. */
static struct futex_bucket *lock_waiter(struct futex_waiter *w, uint64_t *flags) {
    for (;;) {
        struct futex_bucket *b = __atomic_load_n(&w->bucket, __ATOMIC_ACQUIRE);
        *flags = spin_lock_irqsave(&b->lock);
        if (b == __atomic_load_n(&w->bucket, __ATOMIC_ACQUIRE))
            return b;
        spin_unlock_irqrestore(&b->lock, *flags);
    }
}

/* Two bucket locks in address order (one if they are the same bucket). */
static uint64_t lock_pair(struct futex_bucket *a, struct futex_bucket *b) {
    if (a == b)
        return spin_lock_irqsave(&a->lock);
    uint64_t f = spin_lock_irqsave(a < b ? &a->lock : &b->lock);
    spin_lock(a < b ? &b->lock : &a->lock);
    return f;
}

static void unlock_pair(struct futex_bucket *a, struct futex_bucket *b, uint64_t f) {
    if (a != b)
        spin_unlock(&b->lock);
    spin_unlock_irqrestore(&a->lock, f);
}

/* Bucket locked. The waiter only looks at `woken` under the same lock, so
 * it cannot return (and pop its stack frame, or exit) while we still touch
 * it here. */
static int wake_locked(struct futex_bucket *b, struct mm *mm, uint64_t uaddr, int n, uint32_t bitset) {
    int woken = 0;
    for (struct futex_waiter **pp = &b->head; *pp && woken < n;) {
        struct futex_waiter *w = *pp;
        if (w->mm != mm || w->uaddr != uaddr || !(w->bitset & bitset)) {
            pp = &w->next;
            continue;
        }
        *pp = w->next;
        w->woken = true;
        thread_wake(w->t);
        woken++;
    }
    return woken;
}

int futex_wake(struct mm *mm, uint64_t uaddr, int n, uint32_t bitset) {
    struct futex_bucket *b = bucket_for(mm, uaddr);
    uint64_t f = spin_lock_irqsave(&b->lock);
    int woken = wake_locked(b, mm, uaddr, n, bitset);
    spin_unlock_irqrestore(&b->lock, f);
    return woken;
}

/* `deadline` is in monotonic ns; 0 waits without a timeout. */
static int64_t futex_wait(struct mm *mm, uint64_t uaddr, uint32_t val, uint32_t bitset, uint64_t deadline) {
    struct futex_waiter w = {.t = thread_current(), .mm = mm, .uaddr = uaddr, .bitset = bitset};
    struct futex_bucket *b = bucket_for(mm, uaddr);

    /* Check the value and queue up under the bucket lock. A waker changes
     * the value first and then takes this lock to wake, so either we see
     * the new value (EAGAIN) or it sees us on the queue: no lost wakeup. A
     * fault while reading is resolved without sleeping, so it is fine here. */
    uint32_t cur;
    uint64_t f = spin_lock_irqsave(&b->lock);
    int r = copy_from_user(&cur, uaddr, sizeof cur);
    if (r < 0 || cur != val) {
        spin_unlock_irqrestore(&b->lock, f);
        return r < 0 ? r : -EAGAIN;
    }
    enqueue(b, &w);
    spin_unlock_irqrestore(&b->lock, f);

    for (;;) {
        b = lock_waiter(&w, &f);
        int64_t result = 0;
        if (w.woken) {
            spin_unlock_irqrestore(&b->lock, f);
            return 0;
        }
        if (signal_pending_current())
            result = -EINTR;
        else if (deadline && time_monotonic_ns() >= deadline)
            result = -ETIMEDOUT;
        if (result) {
            dequeue(b, &w);
            spin_unlock_irqrestore(&b->lock, f);
            return result;
        }
        spin_unlock_irqrestore(&b->lock, f);
        /* A wake that lands between the unlock and the block marks us
         * wake_pending, so the block returns at once. */
        uint64_t tick = 0;
        if (deadline) {
            uint64_t now = time_monotonic_ns();
            uint64_t left = deadline > now ? deadline - now : 0;
            uint64_t ticks = (left + NS_PER_TICK - 1) / NS_PER_TICK;
            tick = global_ticks + (ticks ? ticks : 1);
        }
        thread_block_until(tick);
    }
}

/* Wakes up to nwake waiters on uaddr and moves up to nrequeue more onto
 * uaddr2 (condition variables hand waiters over to the mutex this way). */
static int64_t futex_requeue(struct mm *mm, uint64_t uaddr, uint64_t uaddr2, int nwake, int nrequeue, bool cmp,
                             uint32_t val3) {
    struct futex_bucket *b1 = bucket_for(mm, uaddr), *b2 = bucket_for(mm, uaddr2);
    uint64_t f = lock_pair(b1, b2);
    if (cmp) {
        uint32_t cur;
        int r = copy_from_user(&cur, uaddr, sizeof cur);
        if (r < 0 || cur != val3) {
            unlock_pair(b1, b2, f);
            return r < 0 ? r : -EAGAIN;
        }
    }
    int woken = wake_locked(b1, mm, uaddr, nwake, FUTEX_BITSET_MATCH_ANY);
    int moved = 0;
    for (struct futex_waiter **pp = &b1->head; *pp && moved < nrequeue;) {
        struct futex_waiter *w = *pp;
        if (w->mm != mm || w->uaddr != uaddr) {
            pp = &w->next;
            continue;
        }
        *pp = w->next;
        w->uaddr = uaddr2;
        enqueue(b2, w); /* if b2 == b1 it goes to the tail, past our scan */
        moved++;
    }
    unlock_pair(b1, b2, f);
    return woken + moved;
}

/* FUTEX_WAKE_OP: atomically apply an operation to *uaddr2, wake waiters on
 * uaddr, and also on uaddr2 if the old value passes a comparison. */
static int64_t futex_wake_op(struct mm *mm, uint64_t uaddr, uint64_t uaddr2, int nwake, int nwake2, uint32_t encoded) {
    int op = (encoded >> 28) & 0xf, cmp = (encoded >> 24) & 0xf;
    int32_t oparg = (int32_t)(encoded << 8) >> 20, cmparg = (int32_t)(encoded << 20) >> 20;
    if (op & 8) { /* FUTEX_OP_OPARG_SHIFT */
        oparg = (int32_t)(1u << (oparg & 31));
        op &= 7;
    }
    if (op > 4 || cmp > 5)
        return -ENOSYS;

    struct futex_bucket *b1 = bucket_for(mm, uaddr), *b2 = bucket_for(mm, uaddr2);
    uint64_t f = lock_pair(b1, b2);
    uint32_t old, cur;
    int r = copy_from_user(&old, uaddr2, sizeof old);
    while (r == 0) {
        uint32_t nv = op == 0 ? (uint32_t)oparg
                      : op == 1 ? old + (uint32_t)oparg
                      : op == 2 ? old | (uint32_t)oparg
                      : op == 3 ? old & ~(uint32_t)oparg
                                : old ^ (uint32_t)oparg;
        r = futex_cmpxchg_user(uaddr2, old, nv, &cur);
        if (r == 0 && cur == old)
            break;
        old = cur; /* lost a race with user space: retry on the new value */
    }
    if (r < 0) {
        unlock_pair(b1, b2, f);
        return r;
    }
    int32_t o = (int32_t)old;
    bool pass = cmp == 0 ? o == cmparg : cmp == 1 ? o != cmparg : cmp == 2 ? o < cmparg
                : cmp == 3 ? o <= cmparg : cmp == 4 ? o > cmparg : o >= cmparg;
    int woken = wake_locked(b1, mm, uaddr, nwake, FUTEX_BITSET_MATCH_ANY);
    if (pass)
        woken += wake_locked(b2, mm, uaddr2, nwake2, FUTEX_BITSET_MATCH_ANY);
    unlock_pair(b1, b2, f);
    return woken;
}

/* Reads a timeout and turns it into a monotonic deadline (0: none).
 * FUTEX_WAIT takes a relative time; FUTEX_WAIT_BITSET an absolute one on
 * CLOCK_MONOTONIC, or CLOCK_REALTIME with FUTEX_CLOCK_REALTIME. */
static int futex_deadline(uint64_t utime, bool absolute, bool realtime, uint64_t *deadline) {
    *deadline = 0;
    if (!utime)
        return 0;
    struct linux_timespec ts;
    if (copy_from_user(&ts, utime, sizeof ts) < 0)
        return -EFAULT;
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000)
        return -EINVAL;
    /* ~317 years, well past any wall-clock date, yet the ns sum below
     * (plus the current time) still fits in 64 bits. */
    if (ts.tv_sec > 10000000000LL)
        ts.tv_sec = 10000000000LL;
    uint64_t ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    uint64_t now = time_monotonic_ns();
    if (!absolute)
        *deadline = now + ns;
    else if (realtime) {
        uint64_t rnow = time_realtime_ns();
        *deadline = ns > rnow ? now + (ns - rnow) : now;
    } else
        *deadline = ns;
    if (!*deadline)
        *deadline = 1; /* an absolute 0 has long passed; 0 would mean "none" */
    return 0;
}

SYSCALL(futex) {
    UNUSED_ARGS();
    uint64_t uaddr = a1, uaddr2 = a5;
    int op = (int)a2, cmd = op & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);
    bool realtime = op & FUTEX_CLOCK_REALTIME;
    uint32_t val = (uint32_t)a3, val3 = (uint32_t)a6;
    /* The count arguments are ints; negative ones mean none. */
    int nr = (int)val < 0 ? 0 : (int)val, nr2 = (int)a4 < 0 ? 0 : (int)a4;
    if (uaddr & 3)
        return -EINVAL;
    if (realtime && cmd != FUTEX_WAIT && cmd != FUTEX_WAIT_BITSET)
        return -ENOSYS;
    struct mm *mm = proc_current()->mm;
    uint64_t deadline;
    int r;

    switch (cmd) {
    case FUTEX_WAIT:
    case FUTEX_WAIT_BITSET:
        if (cmd == FUTEX_WAIT)
            val3 = FUTEX_BITSET_MATCH_ANY;
        if (!val3)
            return -EINVAL;
        if ((r = futex_deadline(a4, cmd == FUTEX_WAIT_BITSET, realtime, &deadline)) < 0)
            return r;
        return futex_wait(mm, uaddr, val, val3, deadline);
    case FUTEX_WAKE:
    case FUTEX_WAKE_BITSET:
        if (cmd == FUTEX_WAKE)
            val3 = FUTEX_BITSET_MATCH_ANY;
        if (!val3)
            return -EINVAL;
        return futex_wake(mm, uaddr, nr, val3);
    case FUTEX_REQUEUE:
    case FUTEX_CMP_REQUEUE:
        if (uaddr2 & 3)
            return -EINVAL;
        return futex_requeue(mm, uaddr, uaddr2, nr, nr2, cmd == FUTEX_CMP_REQUEUE, val3);
    case FUTEX_WAKE_OP:
        if (uaddr2 & 3)
            return -EINVAL;
        return futex_wake_op(mm, uaddr, uaddr2, nr, nr2, val3);
    }
    return -ENOSYS; /* PI futexes and the rest */
}
