#include <asm/elf.h>
#include <kernel/arch.h>
#include <kernel/boot.h>
#include <kernel/cpu.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/proc.h>
#include <kernel/vfs.h>

static struct process kernel_proc = {.pid = 0, .name = "kernel"};
static struct process *all_procs; /* linked through next_all */
static spinlock_t proc_lock = SPINLOCK_INIT;
static int next_pid = 1;

struct process *proc_current(void) {
    struct thread *t = thread_current();
    return t && t->user ? t->user->proc : &kernel_proc;
}

struct regs *current_user_frame(void) {
    return (struct regs *)(thread_kstack_top(thread_current()) - sizeof(struct regs));
}

struct process *proc_find(int pid) {
    uint64_t f = spin_lock_irqsave(&proc_lock);
    struct process *p = all_procs;
    while (p && p->pid != pid)
        p = p->next_all;
    spin_unlock_irqrestore(&proc_lock, f);
    return p;
}

struct process *proc_alloc(struct process *parent);
struct process *proc_alloc(struct process *parent) {
    struct process *p = kzalloc(sizeof *p);
    if (!p)
        return NULL;
    waitq_init(&p->child_wait);
    waitq_init(&p->vfork_wait);
    waitq_init(&p->thread_wait);
    p->state = PROC_ALIVE;
    p->umask = 022;
    p->start_ns = time_monotonic_ns();
    uint64_t f = spin_lock_irqsave(&proc_lock);
    p->pid = next_pid++;
    p->parent = parent;
    if (parent && parent != &kernel_proc) {
        p->sibling = parent->children;
        parent->children = p;
    }
    p->next_all = all_procs;
    all_procs = p;
    spin_unlock_irqrestore(&proc_lock, f);
    return p;
}

/* Thread ids come from the pid space, as on Linux: a tid never collides
 * with a pid, and the first thread of a process has tid == pid. */
int proc_alloc_tid(void) {
    uint64_t f = spin_lock_irqsave(&proc_lock);
    int tid = next_pid++;
    spin_unlock_irqrestore(&proc_lock, f);
    return tid;
}

/* A thread created while its group is being torn down (exit_group, or an
 * execve killing the other threads) starts out killed, so it leaves before
 * running any user code; it still has to join the list for the count that
 * the teardown waits on. */
void proc_add_thread(struct process *p, struct user_thread *ut) {
    struct thread *cur = thread_current();
    uint64_t f = spin_lock_irqsave(&proc_lock);
    if (p->group_exit || (cur->user && cur->user->proc == p && cur->user->killed))
        ut->killed = true;
    ut->next_in_proc = p->threads;
    p->threads = ut;
    p->nthreads++;
    ut->sc_nr = -1;
    spin_unlock_irqrestore(&proc_lock, f);
}

const char *syscall_name(int64_t nr);
int poll_file(struct file *f, struct waitq **wq);

void proc_dump_threads(void) {
    kprintf("threads:\n");
    uint64_t f = spin_lock_irqsave(&proc_lock);
    for (struct process *p = all_procs; p; p = p->next_all)
        for (struct user_thread *ut = p->threads; ut; ut = ut->next_in_proc) {
            if (ut->sc_nr < 0)
                kprintf("  %d/%d %s: running\n", p->pid, ut->tid, p->name);
            else
                kprintf("  %d/%d %s [%s]: in %s(%lx, %lx, %lx)\n", p->pid, ut->tid, p->name,
                        ut->thread ? ut->thread->name : "", syscall_name(ut->sc_nr), ut->sc_a1, ut->sc_a2, ut->sc_a3);
            /* a traced program's main thread: what it did before it got here */
            bool syscall_traced(const char *name);
            if (ut->tid == p->pid && syscall_traced(p->name) && ut->sc_hist_n > 1) {
                int n = ut->sc_hist_n < 16 ? ut->sc_hist_n : 16;
                for (int k = n; k >= 1; k--) {
                    int i = (ut->sc_hist_n - k) % 16;
                    kprintf("      before: %s(%lx, %lx, %lx) = %ld\n", syscall_name(ut->sc_hist[i].nr),
                            ut->sc_hist[i].a1, ut->sc_hist[i].a2, ut->sc_hist[i].a3, ut->sc_hist[i].ret);
                }
            }
            const char *sc = syscall_name(ut->sc_nr);
            if ((!strcmp(sc, "epoll_wait") || !strcmp(sc, "epoll_pwait") || !strcmp(sc, "epoll_pwait2")) && p->fds &&
                ut->sc_a1 < MAX_FDS) {
                void epoll_debug_dump(struct file *f);
                epoll_debug_dump(p->fds->fd[ut->sc_a1]);
            }
            /* blocked reading: what it reads, and whether there is something there now */
            if ((!strcmp(sc, "read") || !strcmp(sc, "recvfrom") || !strcmp(sc, "recvmsg")) && p->fds &&
                ut->sc_a1 < MAX_FDS) {
                struct file *fl = p->fds->fd[ut->sc_a1];
                if (fl && fl->vnode) {
                    struct waitq *wq;
                    int ready = poll_file(fl, &wq);
                    kprintf("    fd %lu: vnode type %d, ops %p, ready %x\n", ut->sc_a1, (int)fl->vnode->type,
                            (void *)fl->vnode->ops, ready);
                    /* who else has it open, and which way */
                    for (struct process *q = all_procs; q; q = q->next_all)
                        if (q->fds)
                            for (int k = 0; k < MAX_FDS; k++) {
                                struct file *o = q->fds->fd[k];
                                if (o && o->vnode == fl->vnode)
                                    kprintf("      held by %d fd %d %s%s\n", q->pid, k,
                                            (o->flags & 3) == 0 ? "read" : (o->flags & 3) == 1 ? "write" : "rdwr",
                                            q->fds->cloexec[k] ? " cloexec" : "");
                            }
                }
            }
        }
    spin_unlock_irqrestore(&proc_lock, f);
}

/* Debug aid (fw_cfg opt/os/hangdump=1): every minute, what each thread
 * is doing, so a hang shows who waits on what. */
static void hangdump_main(void *arg) {
    (void)arg;
    for (;;) {
        thread_sleep_ms(60000);
        proc_dump_threads();
    }
}

uint64_t proc_table_lock(void) { return spin_lock_irqsave(&proc_lock); }
void proc_table_unlock(uint64_t flags) { spin_unlock_irqrestore(&proc_lock, flags); }

int proc_inspect(int pid, void (*fn)(struct process *p, void *arg), void *arg) {
    uint64_t f = spin_lock_irqsave(&proc_lock);
    struct process *p = all_procs;
    while (p && p->pid != pid)
        p = p->next_all;
    if (p)
        fn(p, arg);
    spin_unlock_irqrestore(&proc_lock, f);
    return p ? 0 : -ESRCH;
}

int proc_list_pids(int *pids, int max) {
    int n = 0;
    uint64_t f = spin_lock_irqsave(&proc_lock);
    for (struct process *p = all_procs; p && n < max; p = p->next_all)
        pids[n++] = p->pid;
    spin_unlock_irqrestore(&proc_lock, f);
    return n;
}

int proc_last_pid(void) { return __atomic_load_n(&next_pid, __ATOMIC_RELAXED) - 1; }

/* proc_lock held. Live user threads only (exited ones are unlinked). */
static struct user_thread *thread_find_locked(int tid) {
    for (struct process *p = all_procs; p; p = p->next_all)
        for (struct user_thread *ut = p->threads; ut; ut = ut->next_in_proc)
            if (ut->tid == tid)
                return ut;
    return NULL;
}

/* ---- file descriptor tables ---- */

struct fdtable *fdtable_create(void);
struct fdtable *fdtable_create(void) {
    struct fdtable *t = kzalloc(sizeof *t);
    if (t)
        t->refcount = 1;
    return t;
}

struct fdtable *fdtable_dup(struct fdtable *src);
struct fdtable *fdtable_dup(struct fdtable *src) {
    struct fdtable *t = fdtable_create();
    if (!t)
        return NULL;
    uint64_t f = spin_lock_irqsave(&src->lock);
    for (int i = 0; i < MAX_FDS; i++) {
        if (src->fd[i]) {
            file_ref(src->fd[i]);
            t->fd[i] = src->fd[i];
            t->cloexec[i] = src->cloexec[i];
        }
    }
    spin_unlock_irqrestore(&src->lock, f);
    return t;
}

void fdtable_put(struct fdtable *t);
bool syscall_traced(const char *name);
void fdtable_put(struct fdtable *t) {
    if (__atomic_sub_fetch(&t->refcount, 1, __ATOMIC_ACQ_REL) != 0)
        return;
    for (int i = 0; i < MAX_FDS; i++)
        if (t->fd[i])
            file_put(t->fd[i]);
    kfree(t);
}

/* ---- signals ---- */

static bool sig_default_ignores(int sig) {
    return sig == SIGCHLD || sig == SIGWINCH || sig == SIGCONT || sig == 23 /* SIGURG */ ||
           sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU; /* no job stops yet */
}

static bool sig_discarded(struct process *p, int sig) {
    uint64_t handler = p->sigactions[sig].handler;
    return sig != SIGKILL && (handler == SIG_IGN || (handler == SIG_DFL && sig_default_ignores(sig)));
}

/* The signals of `set` whose delivery would do something (not ignored). */
static uint64_t sigs_wanted(struct process *p, uint64_t set) {
    uint64_t out = 0;
    for (uint64_t s = set; s; s &= s - 1) {
        int sig = __builtin_ctzll(s) + 1;
        if (sig < NSIG && !sig_discarded(p, sig))
            out |= sigbit(sig);
    }
    return out;
}

/* proc_lock held, which also keeps the threads we wake from being freed
 * (a thread leaves the list under the lock before it dies). */
static void kill_locked(struct process *p, int sig) {
    if (!p || p->pid <= 0 || sig <= 0 || sig >= NSIG || p->state != PROC_ALIVE)
        return;
    uint64_t bit = sigbit(sig);
    bool discarded = sig_discarded(p, sig);
    if (discarded) {
        /* As on Linux, an ignored signal is dropped at once unless it is
         * blocked: then it stays pending, since signalfd, sigwait or a new
         * handler installed before it is unblocked may still want it. */
        bool blocked = false;
        for (struct user_thread *ut = p->threads; ut && !blocked; ut = ut->next_in_proc)
            blocked = (ut->sigmask & bit) != 0;
        if (!blocked)
            return;
    }
    __atomic_or_fetch(&p->pending_signals, bit, __ATOMIC_RELEASE);
    if (discarded)
        return; /* nobody needs interrupting for it */
    /* Interrupt the blocking wait of one thread that can take it. If every
     * thread blocks it, it stays pending until one unblocks it. SIGKILL
     * ends the whole group, so wake everyone. */
    for (struct user_thread *ut = p->threads; ut; ut = ut->next_in_proc) {
        if (sig == SIGKILL || !(ut->sigmask & bit)) {
            thread_wake(ut->thread);
            if (sig != SIGKILL)
                break;
        }
    }
}

void proc_kill(struct process *p, int sig) {
    uint64_t f = spin_lock_irqsave(&proc_lock);
    kill_locked(p, sig);
    spin_unlock_irqrestore(&proc_lock, f);
}

int proc_kill_thread(int tgid, int tid, int sig) {
    uint64_t f = spin_lock_irqsave(&proc_lock);
    struct user_thread *ut = thread_find_locked(tid);
    int r = 0;
    if (!ut || (tgid > 0 && ut->proc->pid != tgid) || ut->proc->state != PROC_ALIVE) {
        r = -ESRCH;
    } else if (sig > 0 && sig < NSIG) {
        uint64_t bit = sigbit(sig);
        bool discarded = sig_discarded(ut->proc, sig);
        if (!discarded || (ut->sigmask & bit)) /* blocked: kept, as in kill_locked */
            __atomic_or_fetch(&ut->pending_signals, bit, __ATOMIC_RELEASE);
        if (!discarded)
            thread_wake(ut->thread);
    }
    spin_unlock_irqrestore(&proc_lock, f);
    return r;
}

/* After a handler change: if `sig` is now ignored (SIG_IGN, or SIG_DFL for a
 * signal ignored by default), it is discarded wherever it is pending,
 * blocked or not (POSIX). */
void proc_discard_signal(struct process *p, int sig);
void proc_discard_signal(struct process *p, int sig) {
    uint64_t f = spin_lock_irqsave(&proc_lock);
    if (sig_discarded(p, sig)) {
        __atomic_and_fetch(&p->pending_signals, ~sigbit(sig), __ATOMIC_RELAXED);
        for (struct user_thread *ut = p->threads; ut; ut = ut->next_in_proc)
            __atomic_and_fetch(&ut->pending_signals, ~sigbit(sig), __ATOMIC_RELAXED);
    }
    spin_unlock_irqrestore(&proc_lock, f);
}

int proc_kill_pgrp(int pgid, int sig);
int proc_kill_pgrp(int pgid, int sig) {
    int hits = 0;
    uint64_t f = spin_lock_irqsave(&proc_lock);
    for (struct process *p = all_procs; p; p = p->next_all)
        if (p->pgid == pgid && p->state == PROC_ALIVE) {
            kill_locked(p, sig);
            hits++;
        }
    spin_unlock_irqrestore(&proc_lock, f);
    return hits ? 0 : -ESRCH;
}

bool signal_pending_current(void) {
    struct thread *t = thread_current();
    if (!t || !t->user)
        return false;
    struct user_thread *ut = t->user;
    if (ut->killed)
        return true;
    /* Ignored signals left pending (blocked when they came, see kill_locked)
     * are dropped on the way out without a handler: no reason to interrupt. */
    uint64_t pending = ut->proc->pending_signals | ut->pending_signals;
    return sigs_wanted(ut->proc, pending & (~ut->sigmask | sigbit(SIGKILL))) != 0;
}

/* Linux x86-64 signal frame (what rt_sigreturn and debuggers expect). */
struct sigcontext {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15, rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags;
    uint16_t cs, gs, fs, ss;
    uint64_t err, trapno, oldmask, cr2, fpstate, reserved[8];
};
struct ucontext {
    uint64_t uc_flags, uc_link;
    uint64_t ss_sp;
    int32_t ss_flags, pad;
    uint64_t ss_size;
    struct sigcontext mc;
    uint64_t sigmask;
};
struct siginfo {
    int32_t si_signo, si_errno, si_code, pad;
    uint8_t rest[112];
};
#define FPU_FRAME_SIZE 4096 /* generous: covers AVX-512 XSAVE areas */

static void setup_signal_frame(struct regs *r, int sig, struct linux_sigaction *sa);

/* Called on every return to user mode. */
void signal_deliver(struct regs *r);
void signal_deliver(struct regs *r) {
    struct thread *t = thread_current();
    if (!t->user)
        return;
    struct user_thread *ut = t->user;
    if (ut->killed)
        thread_exit_user(0); /* the group is exiting, or execve wants us gone */
    struct process *p = ut->proc;
    /* Take deliverable signals, lowest first, until one has a handler to run;
     * ignored ones are just dropped (Linux's get_signal loop). */
    for (;;) {
        uint64_t pending = p->pending_signals | ut->pending_signals;
        uint64_t deliverable = pending & (~ut->sigmask | sigbit(SIGKILL));
        if (!deliverable)
            break;
        int sig = __builtin_ctzll(deliverable) + 1;
        uint64_t bit = sigbit(sig);
        /* Our own queue first. A process-wide signal goes to whichever thread
         * claims it first; if another thread beat us to it, look again. */
        if (__atomic_load_n(&ut->pending_signals, __ATOMIC_ACQUIRE) & bit)
            __atomic_and_fetch(&ut->pending_signals, ~bit, __ATOMIC_ACQ_REL);
        else if (!(__atomic_fetch_and(&p->pending_signals, ~bit, __ATOMIC_ACQ_REL) & bit))
            continue;
        if (sig >= NSIG)
            continue;
        struct linux_sigaction *sa = &p->sigactions[sig];

        if (sig == SIGKILL || sa->handler == SIG_DFL) {
            if (sig_default_ignores(sig))
                continue;
            proc_exit(sig); /* terminated by signal */
        }
        if (sa->handler == SIG_IGN)
            continue;
        setup_signal_frame(r, sig, sa);
        return;
    }
    /* No handler ran: a temporary mask (rt_sigsuspend, ppoll...) ends with
     * the system call. */
    if (ut->restore_sigmask) {
        ut->sigmask = ut->saved_sigmask;
        ut->restore_sigmask = false;
    }
}

/* Builds the handler's frame on the user stack and points `r` at the handler. */
static void setup_signal_frame(struct regs *r, int sig, struct linux_sigaction *sa) {
    struct thread *t = thread_current();
    struct user_thread *ut = t->user;

    /* Linux layout, growing down from below the red zone: XSAVE area, then
     * {return address -> restorer, ucontext, siginfo}. At handler entry
     * (rsp + 8) is 16-byte aligned, as if the handler had been called. */
    uint64_t fpstate = ALIGN_DOWN(r->rsp - 128 - FPU_FRAME_SIZE, 64);
    uint64_t frame_size = 8 + sizeof(struct ucontext) + sizeof(struct siginfo);
    uint64_t sp = ALIGN_DOWN(fpstate - frame_size, 16) - 8;
    uint64_t uc_addr = sp + 8;
    uint64_t si_addr = uc_addr + sizeof(struct ucontext);

    struct ucontext uc = {0};
    uc.mc = (struct sigcontext){
        .r8 = r->r8, .r9 = r->r9, .r10 = r->r10, .r11 = r->r11, .r12 = r->r12, .r13 = r->r13,
        .r14 = r->r14, .r15 = r->r15, .rdi = r->rdi, .rsi = r->rsi, .rbp = r->rbp, .rbx = r->rbx,
        .rdx = r->rdx, .rax = r->rax, .rcx = r->rcx, .rsp = r->rsp, .rip = r->rip, .eflags = r->rflags,
        .cs = (uint16_t)r->cs, .ss = (uint16_t)r->ss, .fpstate = fpstate,
    };
    /* sigreturn restores the mask from before the call that installed a
     * temporary one (rt_sigsuspend, ppoll...), not the temporary mask. */
    uc.sigmask = ut->restore_sigmask ? ut->saved_sigmask : ut->sigmask;
    uc.mc.oldmask = uc.sigmask;
    struct siginfo si = {.si_signo = sig};
    if (sig == SIGSYS && ut->sigsys_pending) { /* from seccomp: which call, from where */
        ut->sigsys_pending = false;
        si.si_code = 1; /* SYS_SECCOMP */
        si.si_errno = ut->sigsys_errno;
        uint32_t arch = AUDIT_ARCH;
        memcpy(si.rest, &ut->sigsys_ip, 8);
        memcpy(si.rest + 8, &ut->sigsys_nr, 4);
        memcpy(si.rest + 12, &arch, 4);
    }
    uint64_t restorer = sa->restorer;

    fpu_save(ut->fpu);
    if (!(sa->flags & SA_RESTORER) || copy_to_user(fpstate, ut->fpu, FPU_FRAME_SIZE) < 0 ||
        copy_to_user(uc_addr, &uc, sizeof uc) < 0 || copy_to_user(si_addr, &si, sizeof si) < 0 ||
        copy_to_user(sp, &restorer, 8) < 0)
        proc_exit(SIGSEGV); /* cannot deliver: like Linux, force-kill */

    /* The handler runs with the current mask (the temporary one, if any)
     * plus sa_mask and, unless SA_NODEFER, the signal itself. */
    ut->restore_sigmask = false;
    ut->sigmask |= sa->mask & ~SIG_UNBLOCKABLE;
    if (!(sa->flags & SA_NODEFER))
        ut->sigmask |= sigbit(sig);
    if (sa->flags & SA_RESETHAND)
        sa->handler = SIG_DFL;
    r->rip = sa->handler;
    r->rsp = sp;
    r->rdi = (uint64_t)sig;
    r->rsi = si_addr;
    r->rdx = uc_addr;
    r->rax = 0;
    r->rflags &= ~0x400ULL; /* DF clear per ABI */
}

int64_t signal_return(struct regs *r);
int64_t signal_return(struct regs *r) {
    struct thread *t = thread_current();
    /* The handler's `ret` popped the return address, so rsp is at the
     * ucontext the delivery code wrote. */
    uint64_t uc_addr = r->rsp;
    struct ucontext uc;
    if (copy_from_user(&uc, uc_addr, sizeof uc) < 0)
        proc_exit(SIGSEGV);
    struct sigcontext *m = &uc.mc;
    r->r8 = m->r8, r->r9 = m->r9, r->r10 = m->r10, r->r11 = m->r11, r->r12 = m->r12;
    r->r13 = m->r13, r->r14 = m->r14, r->r15 = m->r15, r->rdi = m->rdi, r->rsi = m->rsi;
    r->rbp = m->rbp, r->rbx = m->rbx, r->rdx = m->rdx, r->rax = m->rax, r->rcx = m->rcx;
    r->rsp = m->rsp, r->rip = m->rip;
    r->rflags = (r->rflags & ~0x40dd5ULL) | (m->eflags & 0x40dd5ULL); /* user-changeable flags only */
    t->user->sigmask = uc.sigmask & ~SIG_UNBLOCKABLE;
    if (m->fpstate && copy_from_user(t->user->fpu, m->fpstate, 512) == 0) {
        /* Restore at least the legacy area; full XSAVE restore needs header validation. */
        fpu_restore(t->user->fpu);
    }
    return (int64_t)r->rax;
}

/* ---- exit and wait ---- */

void fdtable_put(struct fdtable *t);
bool syscall_traced(const char *name);

/* The last thread of `p` is leaving: release what the process holds and
 * turn it into a zombie for its parent to reap. */
__attribute__((noreturn)) static void process_finish(struct process *p, int status) {
    if (p->pid == 1)
        panic("init exited with status %d", status);
    /* With failures traced (fw_cfg opt/os/strace=NAME), how each process of it ended. */
    if (syscall_traced(p->name))
        kprintf("pid %d (%s) ended: %s %d\n", p->pid, p->name, status & 0x7f ? "signal" : "exit code",
                status & 0x7f ? status & 0x7f : (status >> 8) & 0xff);

    /* Detached under the table lock, so /proc never takes a reference on
     * something already being freed. */
    uint64_t lf = spin_lock_irqsave(&proc_lock);
    struct fdtable *fds = p->fds;
    struct path cwd = p->cwd, root = p->root;
    struct mntns *mntns = p->mntns;
    struct ns_set nss = p->ns;
    p->ns = (struct ns_set){.pid = nss.pid};
    nss.pid = NULL;
    struct mm *mm = p->mm;
    struct exec_info *ei = p->exec;
    p->fds = NULL;
    p->cwd = (struct path){0};
    p->root = (struct path){0};
    p->mntns = NULL;
    p->mm = NULL;
    p->exec = NULL;
    spin_unlock_irqrestore(&proc_lock, lf);
    if (fds)
        fdtable_put(fds);
    path_put(&cwd);
    path_put(&root);
    mntns_put(mntns);
    ns_set_put(&nss);
    seccomp_release(p);
    if (mm)
        mm_put(mm); /* no thread has it loaded any more (thread_exit_user) */
    if (ei)
        exec_info_put(ei);

    struct process *init = proc_find(1);
    bool ns_end = pidns_is_reaper(p);
    int ns_reaper = ns_end ? 0 : pidns_reaper(p->ns.pid);
    struct process *reaper = ns_reaper > 0 && ns_reaper != p->pid ? proc_find(ns_reaper) : NULL;
    uint64_t f = spin_lock_irqsave(&proc_lock);
    /* a subreaper among its ancestors, in the same pid namespace */
    for (struct process *a = p->parent; a && a != &kernel_proc; a = a->parent)
        if (a->child_subreaper && a->state == PROC_ALIVE && a->ns.pid == p->ns.pid) {
            reaper = a;
            break;
        }
    if (!reaper || reaper->state != PROC_ALIVE)
        reaper = init;
    if (ns_end) /* its namespace goes with it */
        for (struct process *q = all_procs; q; q = q->next_all)
            if (q != p && q->state == PROC_ALIVE && q->ns.pid && pidns_contains(p->ns.pid, q))
                kill_locked(q, SIGKILL);
    bool init_has_zombie = false;
    while (p->children) {
        struct process *c = p->children;
        p->children = c->sibling;
        c->parent = reaper;
        c->sibling = reaper->children;
        reaper->children = c;
        if (c->state == PROC_ZOMBIE)
            init_has_zombie = true;
        if (c->pdeath_sig && c->state == PROC_ALIVE)
            kill_locked(c, c->pdeath_sig);
    }
    init = reaper;
    p->exit_status = status;
    __atomic_store_n(&p->state, PROC_ZOMBIE, __ATOMIC_RELEASE);
    p->vfork_done = true;
    struct process *parent = p->parent;
    spin_unlock_irqrestore(&proc_lock, f);

    waitq_wake_all(&p->vfork_wait);
    if (init_has_zombie)
        waitq_wake_all(&init->child_wait);
    if (parent) {
        proc_kill(parent, SIGCHLD);
        waitq_wake_all(&parent->child_wait);
    }
    thread_exit();
}

void thread_exit_user(int status) {
    struct thread *t = thread_current();
    struct user_thread *ut = t->user;
    struct process *p = ut->proc;

    /* CLONE_CHILD_CLEARTID: pthread_join sleeps on this word until we are
     * done with the thread's stack and TLS. */
    if (ut->clear_child_tid && p->mm) {
        uint32_t zero = 0;
        if (copy_to_user(ut->clear_child_tid, &zero, sizeof zero) == 0)
            futex_wake(p->mm, ut->clear_child_tid, 1, FUTEX_BITSET_MATCH_ANY);
        ut->clear_child_tid = 0;
    }

    /* Let go of the address space before leaving the group: once we are off
     * the list, the last thread may free the mm while we still run here. */
    ut->exiting = true;
    mm_activate_kernel();

    uint64_t f = spin_lock_irqsave(&proc_lock);
    for (struct user_thread **pp = &p->threads; *pp; pp = &(*pp)->next_in_proc)
        if (*pp == ut) {
            *pp = ut->next_in_proc;
            break;
        }
    p->dead_utime += ut->utime;
    p->dead_stime += ut->stime;
    bool last = --p->nthreads == 0;
    if (!last) /* under the lock: once we drop it, p may be reaped and freed */
        waitq_wake_all(&p->thread_wait);
    spin_unlock_irqrestore(&proc_lock, f);

    if (ut->tid != p->pid)
        pid_unregister(ut->tid);
    if (!last)
        thread_exit();
    process_finish(p, p->group_exit ? p->exit_status : status);
}

/* exit_group, fatal signals, crashes: every thread goes, the first status
 * set is the one the parent sees. */
void proc_exit(int status) {
    struct user_thread *self = thread_current()->user;
    struct process *p = self->proc;
    uint64_t f = spin_lock_irqsave(&proc_lock);
    if (!p->group_exit) {
        p->group_exit = true;
        p->exit_status = status;
    }
    for (struct user_thread *ut = p->threads; ut; ut = ut->next_in_proc)
        if (ut != self) {
            /* It leaves at its next return to user mode; a blocking wait
             * sees signal_pending_current() and returns -EINTR first. A
             * thread running user code is caught by its next timer tick. */
            ut->killed = true;
            thread_wake(ut->thread);
        }
    spin_unlock_irqrestore(&proc_lock, f);
    thread_exit_user(status);
}

static bool only_thread(void *arg) { return __atomic_load_n(&((struct process *)arg)->nthreads, __ATOMIC_ACQUIRE) == 1; }

int proc_kill_other_threads(void) {
    struct user_thread *self = thread_current()->user;
    struct process *p = self->proc;
    uint64_t f = spin_lock_irqsave(&proc_lock);
    for (struct user_thread *ut = p->threads; ut; ut = ut->next_in_proc)
        if (ut != self) {
            ut->killed = true;
            thread_wake(ut->thread);
        }
    spin_unlock_irqrestore(&proc_lock, f);
    /* The others exit at their next kernel exit. Ordinary signals must not
     * make us give up halfway (they stay pending for the new image); only
     * being killed ourselves does. */
    while (!only_thread(p)) {
        if (self->killed)
            return -EINTR;
        if (waitq_wait_cond(&p->thread_wait, only_thread, p) < 0)
            thread_yield();
    }
    self->tid = p->pid; /* like Linux, the survivor takes over the group's id */
    return 0;
}

static bool matches(struct process *c, int pid, struct process *self) {
    if (pid > 0)
        return c->pid == pid;
    if (pid == -1)
        return true;
    if (pid == 0)
        return c->pgid == self->pgid;
    return c->pgid == -pid;
}

struct wait_args {
    struct process *self;
    int pid;
};

static bool has_zombie(void *arg) {
    struct wait_args *w = arg;
    for (struct process *c = w->self->children; c; c = c->sibling)
        if (matches(c, w->pid, w->self) && c->state == PROC_ZOMBIE)
            return true;
    return false;
}

int64_t proc_wait(int pid, int *status, int options);
int64_t proc_wait(int pid, int *status, int options) {
    struct process *self = proc_current();
    for (;;) {
        uint64_t f = spin_lock_irqsave(&proc_lock);
        bool any = false;
        struct process **pp = &self->children, *found = NULL;
        for (; *pp; pp = &(*pp)->sibling) {
            if (!matches(*pp, pid, self))
                continue;
            any = true;
            if ((*pp)->state == PROC_ZOMBIE) {
                found = *pp;
                *pp = found->sibling;
                break;
            }
        }
        if (found) {
            for (struct process **ap = &all_procs; *ap; ap = &(*ap)->next_all)
                if (*ap == found) {
                    *ap = found->next_all;
                    break;
                }
        }
        spin_unlock_irqrestore(&proc_lock, f);

        if (found) {
            int st = found->exit_status, rpid = pid_to_user(found->pid); /* as the waiter sees it, while it still has one */
            pid_unregister(found->pid);
            ns_set_put(&found->ns);
            kfree(found);
            *status = st;
            return rpid;
        }
        if (!any)
            return -ECHILD;
        if (options & WNOHANG)
            return 0;
        struct wait_args w = {self, pid};
        if (waitq_wait_cond(&self->child_wait, has_zombie, &w) < 0)
            return -EINTR;
    }
}

/* ---- faults from user mode ---- */

static bool debug_trace;

static void page_fault(struct regs *r) {
    uint64_t addr = read_cr2();
    if (debug_trace)
        kprintf("#PF addr=%lx rip=%lx err=%lx\n", addr, r->rip, r->error);
    struct thread *t = thread_current();
    struct mm *mm = t && t->user ? t->user->proc->mm : NULL;
    if (mm && mm_handle_fault(mm, addr, r->error & 2, r->error & 16))
        return;
    if (r->cs & 3) {
        kprintf("pid %d (%s): segmentation fault at %lx, rip %lx\n", t->user->proc->pid, t->user->proc->name,
                addr, r->rip);
        proc_exit(SIGSEGV);
    }
    /* A user-memory access from the kernel (copy_from_user and friends):
     * resume at its fixup, which returns -EFAULT. */
    uint64_t fixup = exception_fixup(r->rip);
    if (fixup) {
        r->rip = fixup;
        return;
    }
    kprintf("\nkernel page fault at %lx, rip %lx, error %lx\n", addr, r->rip, r->error);
    panic("page fault in kernel mode");
}

/* Called by isr_dispatch for CPU exceptions raised in user mode. */
void user_exception(struct regs *r);
void user_exception(struct regs *r) {
    struct process *p = proc_current();
    int sig = r->vector == 0 || r->vector == 16 || r->vector == 19 ? SIGFPE
              : r->vector == 6                                     ? SIGILL
              : r->vector == 3 || r->vector == 1                   ? SIGTRAP
                                                                   : SIGSEGV;
    kprintf("pid %d (%s): exception %lu at rip %lx -> signal %d\n", p->pid, p->name, r->vector, r->rip, sig);
    proc_exit(sig);
}

/* ---- init ---- */

static const char *init_path;

int fwcfg_read_file(const char *name, char *buf, size_t size);

static void init_entry(void *arg) {
    (void)arg;
    struct regs *frame = current_user_frame();
    char *argv[] = {(char *)init_path, NULL};
    /* The QEMU test harness asks for test mode through fw_cfg. */
    static char mode[64] = "OS_MODE=";
    char value[32];
    bool test = fwcfg_read_file("opt/os/mode", value, sizeof value) > 0;
    char trace[24];
    int tn = fwcfg_read_file("opt/os/strace", trace, sizeof trace - 1);
    if (tn > 0 && trace[0] == '1') {
        void syscall_set_trace(bool on);
        syscall_set_trace(true);
        debug_trace = true;
    } else if (tn > 0) { /* a program's name: only its failing calls */
        void syscall_trace_failures(const char *name);
        trace[tn] = 0;
        syscall_trace_failures(trace);
    }
    void heap_set_check(bool on);
    if (fwcfg_read_file("opt/os/heapcheck", trace, sizeof trace) > 0 && trace[0] == '1') {
        heap_set_check(true);
        kprintf("heap: checking for use after free\n");
    }
    if (fwcfg_read_file("opt/os/hangdump", trace, sizeof trace) > 0 && trace[0] == '1')
        thread_create("hangdump", hangdump_main, NULL);
    ksnprintf(mode, sizeof mode, "OS_MODE=%s", test ? value : "interactive");
    char *envp[] = {"HOME=/", "PATH=/bin:/sbin:/usr/bin:/usr/sbin", "TERM=linux", mode, NULL};
    int r = do_execve(init_path, argv, envp, frame);
    if (r < 0)
        panic("cannot run %s (error %d)", init_path, r);
    if (debug_trace)
        kprintf("init: entering user mode at rip=%lx rsp=%lx\n", frame->rip, frame->rsp);
    return_to_user(frame);
}

void proc_move_roots(struct mntns *ns, const struct path *old, const struct path *new) {
    struct path drop[64];
    int nd = 0;
    for (;;) {
        bool more = false;
        uint64_t lf = spin_lock_irqsave(&proc_lock);
        for (struct process *p = all_procs; p; p = p->next_all) {
            if (p->mntns != ns)
                continue;
            struct path *places[2] = {&p->root, &p->cwd};
            for (int k = 0; k < 2; k++) {
                if (!path_equal(places[k], old))
                    continue;
                if (nd == 64) {
                    more = true;
                    break;
                }
                drop[nd++] = *places[k];
                *places[k] = *new;
                path_get(places[k]);
            }
        }
        spin_unlock_irqrestore(&proc_lock, lf);
        for (int i = 0; i < nd; i++)
            path_put(&drop[i]);
        nd = 0;
        if (!more)
            break;
    }
}

struct fdtable *fdtable_create(void);

void proc_start_init(const char *path) {
    fs_context(&kernel_proc.root, &kernel_proc.cwd); /* the first namespace's root */
    kernel_proc.mntns = proc_first_mntns();
    ns_init(&kernel_proc.ns);
    idt_set_handler(14, page_fault);
    init_path = path;

    struct process *p = proc_alloc(NULL);
    KASSERT(p && p->pid == 1);
    p->pgid = p->sid = 1;
    p->umask = 022;
    p->mm = mm_create();
    p->fds = fdtable_create();
    KASSERT(p->mm && p->fds);
    p->root = kernel_proc.root;
    path_get(&p->root);
    p->cwd = kernel_proc.root;
    path_get(&p->cwd);
    p->mntns = mntns_get(kernel_proc.mntns);
    ns_set_copy(&p->ns, &kernel_proc.ns);
    ksnprintf(p->name, sizeof p->name, "init");

    struct file *console;
    if (vfs_open(NULL, "/dev/console", O_RDWR, 0, &console) < 0)
        panic("init: no /dev/console");
    for (int i = 0; i < 3; i++) {
        file_ref(console);
        p->fds->fd[i] = console;
    }
    file_put(console);

    struct user_thread *ut = kzalloc(sizeof *ut);
    KASSERT(ut);
    ut->proc = p;
    ut->fpu = fpu_alloc();
    KASSERT(ut->fpu);
    ut->tid = p->pid;
    struct thread *t = thread_create_ex("init", init_entry, NULL, sizeof(struct regs), ut);
    KASSERT(t);
    proc_add_thread(p, ut);
    thread_start_ready(t);
}

/* Whether a process with this pid is running (not exited, not reaped). */
bool proc_alive(int pid) {
    uint64_t f = spin_lock_irqsave(&proc_lock);
    struct process *p = all_procs;
    while (p && p->pid != pid)
        p = p->next_all;
    bool alive = p && p->state == PROC_ALIVE;
    spin_unlock_irqrestore(&proc_lock, f);
    return alive;
}
