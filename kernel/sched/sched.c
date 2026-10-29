#include <kernel/arch.h>
#include <kernel/lib.h>
#include <kernel/boot.h>
#include <kernel/cpu.h>
#include <kernel/mm.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>

/* Preemptive round-robin scheduler with one global run queue. The queue
 * lock is held across switch_context and released by whichever thread runs
 * next (sched_finish), so no other CPU can pick up a thread whose registers
 * have not been saved yet. Per-CPU queues with work stealing come later. */

#define KSTACK_SIZE (32 * 1024)

void switch_context(uint64_t *save_rsp, uint64_t new_rsp);
void thread_start_trampoline(void);
void thread_start(void);

static spinlock_t rq_lock = SPINLOCK_INIT;
static struct thread *rq_head, *rq_tail;
static struct thread *sleepers;
static uint64_t next_id = 1;

static void rq_push(struct thread *t) {
    t->next = NULL;
    if (rq_tail)
        rq_tail->next = t;
    else
        rq_head = t;
    rq_tail = t;
}

static void schedule(enum thread_state new_state);

/* New work is queued: wake one idle CPU right away instead of letting it
 * notice at its next timer tick. */
static void kick_idle_cpu(void) {
    struct cpu *self = this_cpu();
    for (uint32_t i = 0; i < cpu_count; i++) {
        struct cpu *c = cpus[i];
        if (c && c != self && c->online && c->current == c->idle) {
            lapic_send_ipi(c->lapic_id, VEC_RESCHED);
            return;
        }
    }
}

/* Reschedule-IPI handler (interrupts off). */
void sched_preempt(void);
void sched_preempt(void) { schedule(THREAD_READY); }

static struct thread *rq_pop(void) {
    struct thread *t = rq_head;
    if (t) {
        rq_head = t->next;
        if (!rq_head)
            rq_tail = NULL;
        t->next = NULL;
    }
    return t;
}

/* One instruction, through %gs: the thread can be preempted and moved to
 * another CPU at any point, and `this_cpu()->current` (two loads) would then
 * read the CPU it left — someone else's thread. That gave syscalls another
 * process's files and signals: double frees, null user pointers. */
struct thread *thread_current(void) {
    struct thread *t;
    __asm__ volatile("mov %%gs:%c1, %0" : "=r"(t) : "i"(__builtin_offsetof(struct cpu, current)));
    return t;
}

static void thread_free(struct thread *t) {
    if (t->user) {
        fpu_free(t->user->fpu);
        kfree(t->user);
    }
    kfree(t->kstack);
    kfree(t);
}

/* Runs on the new thread right after a switch, with rq_lock held. Loads the
 * per-thread CPU state that switch_context does not carry. */
static void sched_finish(void) {
    struct cpu *c = this_cpu();
    struct thread *prev = c->prev, *cur = c->current;
    c->prev = NULL;
    if (cur != c->idle) {
        uint64_t top = thread_kstack_top(cur);
        c->kernel_rsp = top;
        c->tss.rsp[0] = top;
    }
    struct mm *mm = NULL;
    if (cur->user) {
        wrmsr(MSR_FS_BASE, cur->user->fs_base);
        fpu_restore(cur->user->fpu);
        /* An exiting thread has let go of the mm: the last thread of its
         * process may already be freeing it. */
        if (!cur->user->exiting)
            mm = cur->user->proc->mm;
    }
    /* Never leave a process's page tables loaded under a kernel thread:
     * the process may exit and free them while we still run here. */
    if (mm)
        mm_activate(mm);
    else
        mm_activate_kernel();
    spin_unlock(&rq_lock);
    if (prev && prev->state == THREAD_DEAD)
        thread_free(prev);
}

/* Must be called with local interrupts disabled. `new_state` is what the
 * current thread becomes: READY (preempted or yielding), SLEEPING, BLOCKED
 * or DEAD. Applying it under rq_lock is what makes wakeups race-free. */
static void schedule(enum thread_state new_state) {
    struct cpu *c = this_cpu();
    struct thread *prev = c->current;
    spin_lock(&rq_lock);

    if (prev != c->idle) {
        /* A wake that arrived while we were running (or preempted on the
         * run queue) is consumed by the next attempt to block or sleep. A
         * preemption must not drop it: the thread may be between queueing
         * itself on a futex or wait queue and calling block. */
        if (new_state == THREAD_BLOCKED || new_state == THREAD_SLEEPING) {
            if (prev->wake_pending)
                new_state = THREAD_READY; /* woken before it managed to block */
            prev->wake_pending = false;
        }
        if (new_state == THREAD_SLEEPING) {
            prev->next = sleepers;
            sleepers = prev;
        }
    }

    struct thread *next = rq_pop();
    if (!next) {
        if (prev != c->idle && new_state == THREAD_READY) {
            spin_unlock(&rq_lock); /* nothing else to run: keep going */
            return;
        }
        next = c->idle;
    }
    if (next == prev) {
        spin_unlock(&rq_lock);
        return;
    }
    if (prev != c->idle) {
        prev->state = new_state;
        if (new_state == THREAD_READY)
            rq_push(prev);
    }
    next->state = THREAD_RUNNING;
    next->last_cpu = c->id;
    c->current = next;
    c->prev = prev;
    if (prev->user && new_state != THREAD_DEAD)
        fpu_save(prev->user->fpu);
    switch_context(&prev->rsp, next->rsp);
    sched_finish();
}

void thread_start(void) {
    sched_finish();
    struct thread *t = thread_current();
    __asm__ volatile("sti");
    t->entry(t->arg);
    thread_exit();
}

struct thread *thread_create(const char *name, void (*entry)(void *), void *arg) {
    struct thread *t = thread_create_ex(name, entry, arg, 0, NULL);
    if (t)
        thread_start_ready(t);
    return t;
}

struct thread *thread_create_ex(const char *name, void (*entry)(void *), void *arg, uint64_t reserve,
                                struct user_thread *user) {
    struct thread *t = kzalloc(sizeof *t);
    if (!t)
        return NULL;
    t->kstack = kmalloc(KSTACK_SIZE);
    if (!t->kstack) {
        kfree(t);
        return NULL;
    }
    t->kstack_size = KSTACK_SIZE;
    t->entry = entry;
    t->arg = arg;
    t->last_cpu = -1;
    t->user = user;
    if (user)
        user->thread = t;
    t->switch_reserve = ALIGN_UP(reserve, 16);
    ksnprintf(t->name, sizeof t->name, "%s", name);

    /* Initial frame consumed by switch_context: six callee-saved registers,
     * then the return address. After the ret, rsp is 16-byte aligned, so the
     * trampoline's call enters thread_start with ABI-correct alignment. */
    uint64_t *sp = (uint64_t *)(t->kstack + KSTACK_SIZE - t->switch_reserve);
    *--sp = (uint64_t)thread_start_trampoline;
    for (int i = 0; i < 6; i++)
        *--sp = 0;
    t->rsp = (uint64_t)sp;
    t->state = THREAD_BLOCKED;

    uint64_t f = spin_lock_irqsave(&rq_lock);
    t->id = next_id++;
    spin_unlock_irqrestore(&rq_lock, f);
    return t;
}

void thread_start_ready(struct thread *t) { thread_wake(t); }

void sched_init_cpu(void) {
    struct cpu *c = this_cpu();
    struct thread *idle = kzalloc(sizeof *idle);
    KASSERT(idle);
    ksnprintf(idle->name, sizeof idle->name, "idle%u", c->id);
    idle->state = THREAD_RUNNING;
    idle->last_cpu = c->id;
    c->idle = idle;
    c->current = idle;
}

void thread_yield(void) {
    uint64_t f = irq_save();
    schedule(THREAD_READY);
    irq_restore(f);
}

void thread_exit(void) {
    __asm__ volatile("cli");
    schedule(THREAD_DEAD);
    panic("thread_exit: dead thread was scheduled again");
}

void thread_sleep_ms(uint64_t ms) {
    uint64_t f = irq_save();
    uint64_t ticks = (ms * HZ + 999) / 1000;
    thread_current()->wake_tick = global_ticks + (ticks ? ticks : 1);
    schedule(THREAD_SLEEPING);
    irq_restore(f);
}

void thread_block_current(void) {
    uint64_t f = irq_save();
    schedule(THREAD_BLOCKED);
    irq_restore(f);
}

void thread_block_until(uint64_t tick) {
    if (!tick) {
        thread_block_current();
        return;
    }
    uint64_t f = irq_save();
    thread_current()->wake_tick = tick;
    schedule(THREAD_SLEEPING);
    irq_restore(f);
}

/* Wakes a blocked thread, or a sleeping one before its tick (sleepers
 * re-check their deadline, so an early wakeup only costs a loop). */
void thread_wake(struct thread *t) {
    uint64_t f = spin_lock_irqsave(&rq_lock);
    bool queued = false;
    if (t->state == THREAD_SLEEPING) {
        for (struct thread **pp = &sleepers; *pp; pp = &(*pp)->next)
            if (*pp == t) {
                *pp = t->next;
                break;
            }
    }
    if (t->state == THREAD_BLOCKED || t->state == THREAD_SLEEPING) {
        t->state = THREAD_READY;
        rq_push(t);
        queued = true;
    } else if (t->state == THREAD_RUNNING || t->state == THREAD_READY) {
        /* It may be on its way to blocking (READY: preempted before it got
         * there); its next block or sleep returns at once. */
        t->wake_pending = true;
    }
    spin_unlock_irqrestore(&rq_lock, f);
    if (queued && this_cpu()->online)
        kick_idle_cpu();
}

/* Timer interrupt on every CPU (interrupts are off). */
void sched_tick(void) {
    struct cpu *c = this_cpu();
    c->ticks++;
    if (c->id == 0) {
        uint64_t now = __atomic_add_fetch(&global_ticks, 1, __ATOMIC_RELAXED);
        int woken = 0;
        spin_lock(&rq_lock);
        for (struct thread **pp = &sleepers; *pp;) {
            struct thread *t = *pp;
            if (t->wake_tick <= now) {
                *pp = t->next;
                t->state = THREAD_READY;
                rq_push(t);
                woken++;
            } else {
                pp = &t->next;
            }
        }
        spin_unlock(&rq_lock);
        if (woken > 1) /* this CPU takes one; share the rest */
            kick_idle_cpu();
    }
    schedule(THREAD_READY);
}
