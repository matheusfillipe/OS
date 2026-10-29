#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/proc.h>
#include <kernel/vfs.h>
#include <linux/syscall.h>

#define MAX_EXEC_STRINGS 1024

/* ---- fork / clone ---- */

static bool vfork_done_cond(void *arg) {
    return __atomic_load_n(&((struct process *)arg)->vfork_done, __ATOMIC_ACQUIRE);
}

void signal_deliver(struct regs *r);

static void fork_child_entry(void *arg) {
    (void)arg;
    struct regs *frame = current_user_frame();
    signal_deliver(frame); /* a thread born into a dying group leaves here */
    return_to_user(frame);
}

static void inherit_user_state(struct user_thread *ut, uint64_t flags, uint64_t ctid, uint64_t tls) {
    struct user_thread *cur = thread_current()->user;
    ut->sigmask = cur->sigmask;
    ut->tls = (flags & CLONE_SETTLS) ? tls : cur->tls;
    fpu_save(cur->fpu);
    fpu_copy(ut->fpu, cur->fpu);
    if (flags & CLONE_CHILD_CLEARTID)
        ut->clear_child_tid = ctid;
}

/* The child resumes from the same syscall, on the stack it was given,
 * seeing 0. */
static void setup_child_frame(struct thread *t, uint64_t newsp) {
    struct regs *frame = (struct regs *)(thread_kstack_top(t) - sizeof(struct regs));
    *frame = *current_user_frame();
    arch_fork_child_regs(frame, newsp);
}

/* A new thread in the caller's process. Everything a process holds (mm,
 * descriptors, cwd, signal handlers) is shared by construction, so we ask
 * for the full set of sharing flags; glibc and musl always pass them. */
static int64_t clone_thread(uint64_t flags, uint64_t newsp, uint64_t ptid, uint64_t ctid, uint64_t tls) {
    const uint64_t shared = CLONE_VM | CLONE_SIGHAND | CLONE_FILES | CLONE_FS;
    if ((flags & shared) != shared || (flags & CLONE_VFORK) || (flags & 0x7e0200a0ULL)) /* no new namespaces */
        return -EINVAL;
    struct thread *cur = thread_current();
    struct process *p = cur->user->proc;

    struct user_thread *ut = kzalloc(sizeof *ut);
    if (!ut)
        return -ENOMEM;
    ut->fpu = fpu_alloc();
    struct thread *t = ut->fpu ? thread_create_ex(p->name, fork_child_entry, NULL, sizeof(struct regs), ut) : NULL;
    if (!t) {
        if (ut->fpu)
            fpu_free(ut->fpu);
        kfree(ut);
        return -ENOMEM;
    }
    ut->proc = p;
    ut->tid = proc_alloc_tid();
    pid_register(p->ns.pid, ut->tid);
    inherit_user_state(ut, flags, ctid, tls);
    setup_child_frame(t, newsp);

    int tid = pid_to_user(ut->tid);
    if (flags & CLONE_PARENT_SETTID)
        copy_to_user(ptid, &tid, sizeof tid);
    if (flags & CLONE_CHILD_SETTID)
        copy_to_user(ctid, &tid, sizeof tid); /* same address space as ours */
    proc_add_thread(p, ut);
    thread_start_ready(t);
    return tid;
}

static int64_t do_clone(uint64_t flags, uint64_t newsp, uint64_t ptid, uint64_t ctid, uint64_t tls) {
    if (flags & CLONE_THREAD)
        return clone_thread(flags, newsp, ptid, ctid, tls);
    struct thread *cur = thread_current();
    struct process *parent = cur->user->proc;

    struct process *child = proc_alloc(parent);
    if (!child)
        return -ENOMEM;
    child->pgid = parent->pgid;
    child->sid = parent->sid;
    child->ctty = parent->ctty;
    child->umask = parent->umask;
    child->uid = parent->uid, child->gid = parent->gid, child->euid = parent->euid, child->egid = parent->egid;
    child->cap_eff_off = parent->cap_eff_off, child->cap_perm_off = parent->cap_perm_off, child->cap_inh = parent->cap_inh;
    memcpy(child->sigactions, parent->sigactions, sizeof child->sigactions);
    memcpy(child->name, parent->name, sizeof child->name);
    uint64_t lf = proc_table_lock();
    child->root = parent->root;
    path_get(&child->root);
    child->cwd = parent->cwd;
    path_get(&child->cwd);
    child->mntns = mntns_get(parent->mntns); /* /proc: the parent's image info is shared until either execs */
    child->exec = parent->exec;
    if (child->exec)
        __atomic_add_fetch(&child->exec->refcount, 1, __ATOMIC_RELAXED);
    proc_table_unlock(lf);
    child->vfork_done = !(flags & CLONE_VFORK);
    int nsr = ns_clone(parent, child, flags);
    seccomp_fork(parent, child);

    if (flags & CLONE_FILES) {
        child->fds = parent->fds;
        __atomic_add_fetch(&child->fds->refcount, 1, __ATOMIC_RELAXED);
    } else {
        child->fds = fdtable_dup(parent->fds);
    }
    if (flags & CLONE_VM) {
        child->mm = parent->mm;
        __atomic_add_fetch(&child->mm->refcount, 1, __ATOMIC_RELAXED);
    } else {
        child->mm = mm_clone(parent->mm);
    }

    struct user_thread *ut = kzalloc(sizeof *ut);
    if (ut)
        ut->fpu = fpu_alloc();
    struct thread *t = NULL;
    if (nsr == 0 && child->fds && child->mm && ut && ut->fpu)
        t = thread_create_ex(parent->name, fork_child_entry, NULL, sizeof(struct regs), ut);
    if (!t) {
        /* Undo: the child never ran, so tear it down as a reaped zombie. */
        if (ut) {
            fpu_free(ut->fpu);
            kfree(ut);
        }
        if (child->fds)
            fdtable_put(child->fds);
        if (child->mm)
            mm_put(child->mm);
        path_put(&child->cwd);
        path_put(&child->root);
        mntns_put(child->mntns);
        child->mntns = NULL;
        if (child->exec)
            exec_info_put(child->exec);
        child->exec = NULL;
        mntns_put(child->mntns);
        child->mntns = NULL;
        child->state = PROC_ZOMBIE;
        int st;
        proc_wait(child->pid, &st, WNOHANG);
        return nsr < 0 ? nsr : -ENOMEM;
    }
    ut->proc = child;
    ut->tid = child->pid;
    inherit_user_state(ut, flags, ctid, tls);
    proc_add_thread(child, ut);
    setup_child_frame(t, newsp);

    int pid = pid_to_user(child->pid), cpid = pid_in(child->ns.pid, child->pid);
    if (flags & CLONE_PARENT_SETTID)
        copy_to_user(ptid, &pid, sizeof pid);
    if (flags & CLONE_CHILD_SETTID)
        mm_copy_out(child->mm, ctid, &cpid, sizeof cpid); /* as the child sees itself */

    thread_start_ready(t);

    if (flags & CLONE_VFORK) {
        /* The child borrows our memory until it execs or exits. Only we can
         * reap it, so `child` stays valid while we wait. */
        while (!vfork_done_cond(child))
            waitq_wait_cond(&child->vfork_wait, vfork_done_cond, child);
    }
    return pid;
}

SYSCALL(fork) {
    UNUSED_ARGS();
    return do_clone(SIGCHLD, 0, 0, 0, 0);
}

SYSCALL(vfork) {
    UNUSED_ARGS();
    return do_clone(CLONE_VM | CLONE_VFORK | SIGCHLD, 0, 0, 0, 0);
}

SYSCALL(clone) {
    UNUSED_ARGS();
    /* x86-64 argument order: flags, newsp, parent_tid, child_tid, tls */
    return do_clone(a1, a2, a3, a4, a5);
}

/* clone3 (newer glibc's pthread_create and posix_spawn): the same thing,
 * with the arguments in a versioned struct and the stack as base + size. */
struct clone_args {
    uint64_t flags, pidfd, child_tid, parent_tid, exit_signal, stack, stack_size, tls;
    uint64_t set_tid, set_tid_size, cgroup;
};
#define CLONE_ARGS_SIZE_VER0 64
#define CLONE_PIDFD 0x00001000
#define CLONE_INTO_CGROUP 0x200000000ULL

SYSCALL(clone3) {
    UNUSED_ARGS();
    if (a2 < CLONE_ARGS_SIZE_VER0 || a2 > PAGE_SIZE)
        return -EINVAL;
    struct clone_args ca = {0};
    if (copy_from_user(&ca, a1, a2 < sizeof ca ? a2 : sizeof ca) < 0)
        return -EFAULT;
    if ((ca.flags & (CLONE_PIDFD | CLONE_INTO_CGROUP | CSIGNAL)) || ca.set_tid_size || ca.exit_signal >= NSIG)
        return -EINVAL;
    uint64_t sp = ca.stack ? ca.stack + ca.stack_size : 0;
    return do_clone((ca.flags & 0xffffffffULL) | ca.exit_signal, sp, ca.parent_tid, ca.child_tid, ca.tls);
}

/* ---- execve ---- */

static void free_strings(char **v) {
    if (!v)
        return;
    for (int i = 0; v[i]; i++)
        kfree(v[i]);
    kfree(v);
}

static int64_t copy_strings(uint64_t uvec, char ***out) {
    char **v = kzalloc(sizeof(char *) * (MAX_EXEC_STRINGS + 1));
    if (!v)
        return -ENOMEM;
    for (int i = 0; uvec; i++) {
        if (i == MAX_EXEC_STRINGS) {
            free_strings(v);
            return -E2BIG;
        }
        uint64_t p;
        if (copy_from_user(&p, uvec + (uint64_t)i * 8, 8) < 0) {
            free_strings(v);
            return -EFAULT;
        }
        if (!p)
            break;
        char *s;
        int64_t r = user_path(p, &s);
        if (r < 0) {
            free_strings(v);
            return r;
        }
        v[i] = s;
    }
    *out = v;
    return 0;
}

SYSCALL(execve) {
    UNUSED_ARGS();
    char *path, **argv = NULL, **envp = NULL;
    int64_t res = user_path(a1, &path);
    if (res < 0)
        return res;
    if ((res = copy_strings(a2, &argv)) == 0 && (res = copy_strings(a3, &envp)) == 0)
        res = do_execve(path, argv, envp, frame);
    free_strings(argv);
    free_strings(envp);
    kfree(path);
    return res < 0 ? res : (int64_t)regs_return(frame); /* success: keep the new frame's return value (0) */
}

/* ---- exit / wait ---- */

SYSCALL(exit) {
    UNUSED_ARGS();
    thread_exit_user(((int)a1 & 0xff) << 8); /* just this thread */
}

SYSCALL(exit_group) {
    UNUSED_ARGS();
    proc_exit(((int)a1 & 0xff) << 8);
}

SYSCALL(wait4) {
    UNUSED_ARGS();
    int status = 0, want = (int)a1;
    if (want > 0 && !(want = pid_from_user(want)))
        return -ECHILD;
    if (want < -1 && !(want = -pid_from_user(-want)))
        return -ECHILD;
    int64_t pid = proc_wait(want, &status, (int)a3);
    if (pid > 0 && a2 && copy_to_user(a2, &status, sizeof status) < 0)
        return -EFAULT;
    if (pid > 0 && a4) {
        uint8_t ru[144] = {0};
        copy_to_user(a4, ru, sizeof ru);
    }
    return pid;
}

/* ---- signals ---- */

SYSCALL(kill) {
    UNUSED_ARGS();
    int pid = (int)a1, sig = (int)a2;
    if (sig < 0 || sig >= NSIG)
        return -EINVAL;
    if (pid > 0) {
        struct process *p = proc_find(pid_from_user(pid));
        if (!p || p->state != PROC_ALIVE)
            return -ESRCH;
        if (sig)
            proc_kill(p, sig);
        return 0;
    }
    int pgid = pid == 0 ? proc_current()->pgid : pid == -1 ? -1 : pid_from_user(-pid);
    if (!pgid)
        return -ESRCH;
    if (pgid == -1)
        return -EPERM; /* broadcast kill: not yet */
    return sig ? proc_kill_pgrp(pgid, sig) : 0;
}

SYSCALL(tgkill) {
    UNUSED_ARGS();
    if ((int)a1 <= 0 || (int)a2 <= 0)
        return -EINVAL;
    if ((int)a3 < 0 || (int)a3 >= NSIG)
        return -EINVAL;
    int tgid = pid_from_user((int)a1), tid = pid_from_user((int)a2);
    if (!tgid || !tid)
        return -ESRCH;
    return proc_kill_thread(tgid, tid, (int)a3);
}

SYSCALL(tkill) {
    UNUSED_ARGS();
    if ((int)a1 <= 0 || (int)a2 < 0 || (int)a2 >= NSIG)
        return -EINVAL;
    int tid = pid_from_user((int)a1);
    return tid ? proc_kill_thread(0, tid, (int)a2) : -ESRCH;
}

SYSCALL(rt_sigaction) {
    UNUSED_ARGS();
    int sig = (int)a1;
    if (sig <= 0 || sig >= NSIG || a4 != 8)
        return -EINVAL;
    struct process *p = proc_current();
    struct linux_sigaction old = p->sigactions[sig];
    if (a2) {
        if (sig == SIGKILL || sig == SIGSTOP)
            return -EINVAL;
        struct linux_sigaction sa;
        if (copy_from_user(&sa, a2, sizeof sa) < 0)
            return -EFAULT;
        sa.mask &= ~SIG_UNBLOCKABLE;
        p->sigactions[sig] = sa;
        proc_discard_signal(p, sig); /* if it is ignored now */
    }
    if (a3 && copy_to_user(a3, &old, sizeof old) < 0)
        return -EFAULT;
    return 0;
}

SYSCALL(rt_sigprocmask) {
    UNUSED_ARGS();
    if (a4 != 8)
        return -EINVAL;
    struct user_thread *u = thread_current()->user;
    uint64_t old = u->sigmask;
    if (a2) {
        uint64_t set;
        if (copy_from_user(&set, a2, 8) < 0)
            return -EFAULT;
        if (a1 == SIG_BLOCK)
            u->sigmask |= set;
        else if (a1 == SIG_UNBLOCK)
            u->sigmask &= ~set;
        else if (a1 == SIG_SETMASK)
            u->sigmask = set;
        else
            return -EINVAL;
        u->sigmask &= ~SIG_UNBLOCKABLE;
    }
    if (a3 && copy_to_user(a3, &old, 8) < 0)
        return -EFAULT;
    return 0;
}

SYSCALL(rt_sigreturn) {
    UNUSED_ARGS();
    return signal_return(frame);
}

SYSCALL(rt_sigpending) {
    UNUSED_ARGS();
    struct user_thread *u = thread_current()->user;
    uint64_t pending = (u->proc->pending_signals | u->pending_signals) & u->sigmask;
    return copy_to_user(a1, &pending, 8);
}

SYSCALL(sigaltstack) {
    UNUSED_ARGS();
    if (a2) {
        uint8_t ss[24] = {0};
        ss[8] = 2; /* SS_DISABLE */
        return copy_to_user(a2, ss, sizeof ss);
    }
    return 0;
}

static bool never(void *arg) {
    (void)arg;
    return false;
}

SYSCALL(pause) {
    UNUSED_ARGS();
    struct waitq wq;
    waitq_init(&wq);
    while (waitq_wait_cond(&wq, never, NULL) == 0)
        ;
    return -EINTR;
}

int sigmask_push(uint64_t uset, uint64_t size) {
    if (size != 8)
        return -EINVAL;
    uint64_t set;
    if (copy_from_user(&set, uset, 8) < 0)
        return -EFAULT;
    struct user_thread *u = thread_current()->user;
    u->saved_sigmask = u->sigmask;
    u->restore_sigmask = true;
    u->sigmask = set & ~SIG_UNBLOCKABLE;
    return 0;
}

void sigmask_pop(int64_t result) {
    struct user_thread *u = thread_current()->user;
    if (result == -EINTR || !u->restore_sigmask)
        return; /* the signal that interrupted us runs under the temporary mask */
    u->sigmask = u->saved_sigmask;
    u->restore_sigmask = false;
}

/* Waits for a signal under a temporary mask. The handler runs with that
 * mask; its frame holds the original one, which sigreturn puts back (or
 * signal_deliver does at syscall exit, if no handler runs). */
SYSCALL(rt_sigsuspend) {
    UNUSED_ARGS();
    int r = sigmask_push(a1, a2);
    if (r < 0)
        return r;
    struct waitq wq;
    waitq_init(&wq);
    while (waitq_wait_cond(&wq, never, NULL) == 0)
        ;
    return -EINTR;
}

/* ---- ids and sessions ---- */

SYSCALL(getpid) {
    UNUSED_ARGS();
    return pid_to_user(proc_current()->pid);
}

SYSCALL(gettid) {
    UNUSED_ARGS();
    return pid_to_user(thread_current()->user->tid);
}

SYSCALL(getppid) {
    UNUSED_ARGS();
    struct process *p = proc_current();
    return p->parent ? pid_to_user(p->parent->pid) : 0;
}

SYSCALL(getuid) {
    UNUSED_ARGS();
    return uid_to_user(proc_current()->uid);
}

SYSCALL(geteuid) {
    UNUSED_ARGS();
    return uid_to_user(proc_current()->euid);
}

SYSCALL(getgid) {
    UNUSED_ARGS();
    return gid_to_user(proc_current()->gid);
}

SYSCALL(getegid) {
    UNUSED_ARGS();
    return gid_to_user(proc_current()->egid);
}

/* There are no saved set-IDs yet: the saved one is the effective one. */
static int64_t put_res_ids(uint64_t ureal, uint64_t ueff, uint64_t usaved, uint32_t real, uint32_t eff) {
    if (copy_to_user(ureal, &real, 4) < 0 || copy_to_user(ueff, &eff, 4) < 0 || copy_to_user(usaved, &eff, 4) < 0)
        return -EFAULT;
    return 0;
}

SYSCALL(getresuid) {
    UNUSED_ARGS();
    struct process *p = proc_current();
    return put_res_ids(a1, a2, a3, uid_to_user(p->uid), uid_to_user(p->euid));
}

SYSCALL(getresgid) {
    UNUSED_ARGS();
    struct process *p = proc_current();
    return put_res_ids(a1, a2, a3, gid_to_user(p->gid), gid_to_user(p->egid));
}

bool proc_has_cap(int cap);
static void caps_after_setuid(struct process *p, uint32_t old_uid, uint32_t old_euid);

SYSCALL(setuid) {
    UNUSED_ARGS();
    struct process *p = proc_current();
    uint32_t k;
    if (!uid_from_user((uint32_t)a1, &k))
        return -EINVAL; /* no such user in its namespace */
    uint32_t old_uid = p->uid, old_euid = p->euid;
    if (proc_has_cap(7)) /* CAP_SETUID: any user, for good */
        p->uid = p->euid = k;
    else if (k == p->uid || k == p->euid)
        p->euid = k;
    else
        return -EPERM;
    caps_after_setuid(p, old_uid, old_euid);
    return 0;
}

SYSCALL(setgid) {
    UNUSED_ARGS();
    struct process *p = proc_current();
    uint32_t k;
    if (!gid_from_user((uint32_t)a1, &k))
        return -EINVAL;
    if (proc_has_cap(6)) /* CAP_SETGID */
        p->gid = p->egid = k;
    else if (k == p->gid || k == p->egid)
        p->egid = k;
    else
        return -EPERM;
    return 0;
}

/* setresuid/setreuid and the group versions. There is no separate saved id
 * here: it is the real one. Without CAP_SETUID each new id must be one the
 * process already has; -1 keeps one. */
static int64_t set_ids(uint32_t *real, uint32_t *eff, int64_t r, int64_t e, bool user) {
    uint32_t kr = *real, ke = *eff;
    if ((uint32_t)r != (uint32_t)-1 && !(user ? uid_from_user((uint32_t)r, &kr) : gid_from_user((uint32_t)r, &kr)))
        return -EINVAL;
    if ((uint32_t)e != (uint32_t)-1 && !(user ? uid_from_user((uint32_t)e, &ke) : gid_from_user((uint32_t)e, &ke)))
        return -EINVAL;
    if (!proc_has_cap(user ? 7 : 6) && ((kr != *real && kr != *eff) || (ke != *real && ke != *eff)))
        return -EPERM;
    *real = kr;
    *eff = ke;
    return 0;
}

SYSCALL(setresuid) {
    UNUSED_ARGS();
    struct process *p = proc_current();
    uint32_t old_uid = p->uid, old_euid = p->euid;
    int64_t r = set_ids(&p->uid, &p->euid, (int64_t)a1, (int64_t)a2, true);
    if (r == 0)
        caps_after_setuid(p, old_uid, old_euid);
    return r;
}

SYSCALL(setreuid) {
    UNUSED_ARGS();
    struct process *p = proc_current();
    uint32_t old_uid = p->uid, old_euid = p->euid;
    int64_t r = set_ids(&p->uid, &p->euid, (int64_t)a1, (int64_t)a2, true);
    if (r == 0)
        caps_after_setuid(p, old_uid, old_euid);
    return r;
}

SYSCALL(setresgid) {
    UNUSED_ARGS();
    struct process *p = proc_current();
    return set_ids(&p->gid, &p->egid, (int64_t)a1, (int64_t)a2, false);
}

SYSCALL(setregid) {
    UNUSED_ARGS();
    struct process *p = proc_current();
    return set_ids(&p->gid, &p->egid, (int64_t)a1, (int64_t)a2, false);
}

/* The ids used for file access. There are no separate ones here: they are
 * the effective ids, and asking for a change returns the old one as Linux
 * does (bash calls these at start). */
SYSCALL(setfsuid) {
    UNUSED_ARGS();
    return uid_to_user(proc_current()->euid);
}

SYSCALL(setfsgid) {
    UNUSED_ARGS();
    return gid_to_user(proc_current()->egid);
}
/* Capabilities (0..40). Root has them all, an ordinary user none; a new
 * user namespace gives its creator all of them there; exec recomputes them
 * from the user (kernel/proc/exec.c). Stored as the bits not held. */
#define CAP_ALL ((1ull << 41) - 1)

struct cap_header {
    uint32_t version;
    int32_t pid;
};
struct cap_data {
    uint32_t effective, permitted, inheritable;
};

static int cap_header_in(uint64_t uhdr, struct cap_header *h, size_t *n) {
    if (copy_from_user(h, uhdr, sizeof *h) < 0)
        return -EFAULT;
    if (h->version != 0x19980330 && h->version != 0x20071026 && h->version != 0x20080522) {
        h->version = 0x20080522; /* the version we speak, as Linux answers */
        return copy_to_user(uhdr, h, sizeof *h) < 0 ? -EFAULT : -EINVAL;
    }
    *n = h->version == 0x19980330 ? 1 : 2;
    return 0;
}

SYSCALL(capget) {
    UNUSED_ARGS();
    struct cap_header h;
    size_t n;
    int r = cap_header_in(a1, &h, &n);
    if (r < 0)
        return r == -EINVAL && !a2 ? 0 : r;
    if (!a2)
        return 0;
    struct process *p = h.pid ? proc_find(pid_from_user(h.pid)) : proc_current();
    if (!p)
        return -ESRCH;
    uint64_t eff = CAP_ALL & ~p->cap_eff_off, perm = CAP_ALL & ~p->cap_perm_off, inh = p->cap_inh & CAP_ALL;
    struct cap_data d[2] = {{(uint32_t)eff, (uint32_t)perm, (uint32_t)inh},
                            {(uint32_t)(eff >> 32), (uint32_t)(perm >> 32), (uint32_t)(inh >> 32)}};
    return copy_to_user(a2, d, sizeof d[0] * n) < 0 ? -EFAULT : 0;
}

SYSCALL(capset) {
    UNUSED_ARGS();
    struct cap_header h;
    size_t n;
    int r = cap_header_in(a1, &h, &n);
    if (r < 0)
        return r;
    struct process *p = proc_current();
    if (h.pid && pid_from_user(h.pid) != p->pid)
        return -EPERM;
    struct cap_data d[2] = {0};
    if (copy_from_user(d, a2, sizeof d[0] * n) < 0)
        return -EFAULT;
    uint64_t eff = (d[0].effective | (uint64_t)d[1].effective << 32) & CAP_ALL;
    uint64_t perm = (d[0].permitted | (uint64_t)d[1].permitted << 32) & CAP_ALL;
    uint64_t inh = (d[0].inheritable | (uint64_t)d[1].inheritable << 32) & CAP_ALL;
    uint64_t had = CAP_ALL & ~p->cap_perm_off;
    if ((perm & ~had) || (eff & ~perm))
        return -EPERM; /* none can be gained, and effective ones must be permitted */
    p->cap_perm_off = CAP_ALL & ~perm;
    p->cap_eff_off = CAP_ALL & ~eff;
    p->cap_inh = inh;
    return 0;
}

bool proc_has_cap(int cap) { return !(proc_current()->cap_eff_off & (1ull << cap)); }

/* After the user ids change, as Linux: leaving root (all three non-zero)
 * drops the capabilities (but PR_SET_KEEPCAPS keeps the permitted ones);
 * becoming root again makes the permitted ones effective. */
static void caps_after_setuid(struct process *p, uint32_t old_uid, uint32_t old_euid) {
    bool was_root = uid_to_user(old_uid) == 0 || uid_to_user(old_euid) == 0;
    if (was_root && uid_to_user(p->uid) != 0 && uid_to_user(p->euid) != 0) {
        if (!p->keep_caps)
            p->cap_perm_off = CAP_ALL;
        p->cap_eff_off = CAP_ALL;
    } else if (uid_to_user(old_euid) == 0 && uid_to_user(p->euid) != 0) {
        p->cap_eff_off = CAP_ALL;
    } else if (uid_to_user(old_euid) != 0 && uid_to_user(p->euid) == 0) {
        p->cap_eff_off = p->cap_perm_off;
    }
}


/* Supplementary groups: there are none here, so a request to set them
 * (programs dropping root clear them first) is accepted. */
SYSCALL(setgroups) {
    UNUSED_ARGS();
    return a1 > 65536 ? -EINVAL : 0;
}

/* Scheduling priorities: everything here runs at the normal priority with
 * the normal policy. Asking is answered so, and asking for a change is
 * accepted (browsers' threads ask about both at start). */
SYSCALL(getpriority) {
    UNUSED_ARGS();
    return 20; /* the kernel's way of saying nice 0 */
}

SYSCALL(setpriority) {
    UNUSED_ARGS();
    return 0;
}

SYSCALL(sched_getparam) {
    UNUSED_ARGS();
    int32_t prio = 0;
    return a2 && copy_to_user(a2, &prio, sizeof prio) < 0 ? -EFAULT : 0;
}

SYSCALL(sched_setparam) {
    UNUSED_ARGS();
    return 0;
}

SYSCALL(sched_getscheduler) {
    UNUSED_ARGS();
    return 0; /* SCHED_OTHER */
}

SYSCALL(sched_setscheduler) {
    UNUSED_ARGS();
    return 0;
}

SYSCALL(getgroups) {
    UNUSED_ARGS();
    return 0;
}

SYSCALL(setpgid) {
    UNUSED_ARGS();
    struct process *p = a1 ? proc_find(pid_from_user((int)a1)) : proc_current();
    if (!p)
        return -ESRCH;
    int pgid = a2 ? pid_from_user((int)a2) : p->pid;
    if (!pgid)
        return -EPERM;
    p->pgid = pgid;
    return 0;
}

SYSCALL(getpgid) {
    UNUSED_ARGS();
    struct process *p = a1 ? proc_find(pid_from_user((int)a1)) : proc_current();
    return p ? pid_to_user(p->pgid) : -ESRCH;
}

SYSCALL(getpgrp) {
    UNUSED_ARGS();
    return pid_to_user(proc_current()->pgid);
}

SYSCALL(setsid) {
    UNUSED_ARGS();
    struct process *p = proc_current();
    if (p->pgid == p->pid)
        return -EPERM;
    p->sid = p->pgid = p->pid;
    p->ctty = -1; /* a new session starts without a terminal */
    return pid_to_user(p->sid);
}

SYSCALL(getsid) {
    UNUSED_ARGS();
    struct process *p = a1 ? proc_find(pid_from_user((int)a1)) : proc_current();
    return p ? pid_to_user(p->sid) : -ESRCH;
}

/* ---- thread-local storage, limits, misc process state ---- */

SYSCALL(set_tid_address) {
    UNUSED_ARGS();
    thread_current()->user->clear_child_tid = a1;
    return pid_to_user(thread_current()->user->tid);
}

SYSCALL(prlimit64) {
    UNUSED_ARGS();
    if (a4) {
        struct linux_rlimit lim = {UINT64_MAX, UINT64_MAX};
        if (a2 == 3) /* RLIMIT_STACK */
            lim.rlim_cur = USER_STACK_SIZE;
        else if (a2 == 7) /* RLIMIT_NOFILE */
            lim.rlim_cur = lim.rlim_max = MAX_FDS;
        return copy_to_user(a4, &lim, sizeof lim);
    }
    return 0;
}

SYSCALL(getrlimit) {
    UNUSED_ARGS();
    return sys_prlimit64(0, a1, 0, a2, 0, 0, frame);
}

SYSCALL(setrlimit) {
    UNUSED_ARGS();
    return 0;
}

SYSCALL(getrusage) {
    UNUSED_ARGS();
    uint8_t ru[144] = {0};
    return copy_to_user(a2, ru, sizeof ru);
}

SYSCALL(times) {
    UNUSED_ARGS();
    if (a1) {
        uint64_t t[4] = {0};
        if (copy_to_user(a1, t, sizeof t) < 0)
            return -EFAULT;
    }
    return (int64_t)(time_monotonic_ns() / 10000000ULL); /* clock ticks at 100 Hz */
}

SYSCALL(prctl) {
    UNUSED_ARGS();
    /* Names belong to threads, as on Linux; the main thread's is also the
     * process name we log. */
    struct thread *t = thread_current();
    if (a1 == 15) { /* PR_SET_NAME */
        char name[16];
        int64_t n = strncpy_from_user(name, a2, sizeof name);
        if (n == -EFAULT)
            return -EFAULT;
        name[15] = 0; /* longer names are truncated */
        ksnprintf(t->name, sizeof t->name, "%s", name);
        if (t->user->tid == proc_current()->pid)
            memcpy(proc_current()->name, name, sizeof name);
        return 0;
    }
    if (a1 == 16) { /* PR_GET_NAME */
        char name[16] = {0};
        ksnprintf(name, sizeof name, "%s", t->name);
        return copy_to_user(a2, name, sizeof name);
    }
    struct process *p = proc_current();
    switch (a1) {
    case 1: /* PR_SET_PDEATHSIG: a signal when the parent goes */
        if (a2 >= NSIG)
            return -EINVAL;
        p->pdeath_sig = (int)a2;
        return 0;
    case 2: /* PR_GET_PDEATHSIG */
        return copy_to_user(a2, &p->pdeath_sig, sizeof p->pdeath_sig);
    case 3: /* PR_GET_DUMPABLE */
        return p->not_dumpable ? 0 : 1;
    case 4: /* PR_SET_DUMPABLE */
        if (a2 > 1)
            return -EINVAL;
        p->not_dumpable = !a2;
        return 0;
    case 7:  /* PR_GET_KEEPCAPS */
        return p->keep_caps;
    case 8:  /* PR_SET_KEEPCAPS */
        p->keep_caps = a2 != 0;
        return 0;
    case 21: /* PR_GET_SECCOMP */
        return seccomp_mode();
    case 22: /* PR_SET_SECCOMP */
        /* prctl numbers the modes 1 (strict) and 2 (filter); seccomp(2) 0 and 1 */
        return a2 == 1 || a2 == 2 ? seccomp_set(a2 - 1, 0, a3) : -EINVAL;
    case 23: /* PR_CAPBSET_READ: every capability is in the bounding set */
        return a2 < 41 ? 1 : -EINVAL;
    case 24: /* PR_CAPBSET_DROP */
        return a2 < 41 ? 0 : -EINVAL;
    case 36: /* PR_SET_CHILD_SUBREAPER */
        p->child_subreaper = a2 != 0;
        return 0;
    case 37: { /* PR_GET_CHILD_SUBREAPER */
        int v = p->child_subreaper;
        return copy_to_user(a2, &v, sizeof v);
    }
    case 38: /* PR_SET_NO_NEW_PRIVS: once set, for good */
        if (a2 != 1 || a3 || a4 || a5)
            return -EINVAL;
        p->no_new_privs = true;
        return 0;
    case 39: /* PR_GET_NO_NEW_PRIVS */
        return p->no_new_privs;
    case 47: /* PR_CAP_AMBIENT: raise, lower, clear all succeed; is-set says no */
        return a2 == 1 ? 0 : (a2 == 2 || a2 == 3 || a2 == 4) ? 0 : -EINVAL;
    case 0x59616d61: /* PR_SET_PTRACER (Yama) */
        return 0;
    }
    return -EINVAL;
}

SYSCALL(set_robust_list) {
    UNUSED_ARGS();
    return 0;
}

SYSCALL(sched_yield) {
    UNUSED_ARGS();
    thread_yield();
    return 0;
}

/* Every thread may run on every CPU (one global run queue). Like Linux, the
 * kernel's mask is a whole number of longs covering the CPUs, and the call
 * returns its size; the caller's buffer must hold it. */
SYSCALL(sched_getaffinity) {
    UNUSED_ARGS();
    uint8_t mask[MAX_CPUS / 8] = {0};
    size_t size = ALIGN_UP(cpu_count, 64) / 8;
    if (a2 < size || (a2 & 7))
        return -EINVAL;
    for (uint32_t i = 0; i < cpu_count; i++)
        mask[i / 8] |= (uint8_t)(1 << (i % 8));
    if (copy_to_user(a3, mask, size) < 0)
        return -EFAULT;
    return (int64_t)size;
}

/* Accepted and ignored, as long as the mask names at least one CPU we have:
 * the scheduler has no per-thread placement yet. */
SYSCALL(sched_setaffinity) {
    UNUSED_ARGS();
    uint8_t mask[MAX_CPUS / 8] = {0};
    size_t n = a2 < sizeof mask ? a2 : sizeof mask;
    if (copy_from_user(mask, a3, n) < 0)
        return -EFAULT;
    for (uint32_t i = 0; i < cpu_count && i / 8 < n; i++)
        if (mask[i / 8] & (1 << (i % 8)))
            return 0;
    return -EINVAL;
}
