#include <asm/elf.h>
#include <asm/regs.h>
#include <asm/syscall_table.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/ns.h>
#include <kernel/proc.h>
#include <linux/abi.h>
#include <linux/syscall.h>

/* seccomp: a process limits which system calls it may make, with small
 * classic-BPF programs run on every call (Chromium's renderer sandbox,
 * Flatpak's and bubblewrap's filters). Filters stack: every installed one
 * runs and the strictest answer wins; children inherit them, and they
 * survive execve. Filters belong to the whole process here (as if every
 * install used SECCOMP_FILTER_FLAG_TSYNC). */

struct sock_filter {
    uint16_t code;
    uint8_t jt, jf;
    uint32_t k;
};

struct sock_fprog {
    uint16_t len;
    uint8_t pad[6];
    uint64_t filter; /* user pointer */
};

struct seccomp_data {
    int32_t nr;
    uint32_t arch;
    uint64_t instruction_pointer;
    uint64_t args[6];
};

struct seccomp_filter {
    int refcount;
    struct seccomp_filter *prev; /* referenced */
    bool log;
    int len;
    struct sock_filter insns[];
};

#define BPF_MAXINSNS 4096

#define SECCOMP_RET_KILL_PROCESS 0x80000000U
#define SECCOMP_RET_KILL_THREAD  0x00000000U
#define SECCOMP_RET_TRAP         0x00030000U
#define SECCOMP_RET_ERRNO        0x00050000U
#define SECCOMP_RET_USER_NOTIF   0x7fc00000U
#define SECCOMP_RET_TRACE        0x7ff00000U
#define SECCOMP_RET_LOG          0x7ffc0000U
#define SECCOMP_RET_ALLOW        0x7fff0000U
#define SECCOMP_RET_ACTION_FULL  0xffff0000U
#define SECCOMP_RET_DATA         0x0000ffffU

#define SECCOMP_SET_MODE_STRICT  0
#define SECCOMP_SET_MODE_FILTER  1
#define SECCOMP_GET_ACTION_AVAIL 2
#define SECCOMP_GET_NOTIF_SIZES  3

#define SECCOMP_FILTER_FLAG_TSYNC        1
#define SECCOMP_FILTER_FLAG_LOG          2
#define SECCOMP_FILTER_FLAG_SPEC_ALLOW   4
#define SECCOMP_FILTER_FLAG_NEW_LISTENER 8
#define SECCOMP_FILTER_FLAG_TSYNC_ESRCH  16

/* classic BPF */
#define BPF_CLASS(c) ((c)&0x07)
#define BPF_LD   0x00
#define BPF_LDX  0x01
#define BPF_ST   0x02
#define BPF_STX  0x03
#define BPF_ALU  0x04
#define BPF_JMP  0x05
#define BPF_RET  0x06
#define BPF_MISC 0x07
#define BPF_SIZE(c) ((c)&0x18)
#define BPF_W 0x00
#define BPF_MODE(c) ((c)&0xe0)
#define BPF_IMM 0x00
#define BPF_ABS 0x20
#define BPF_MEM 0x60
#define BPF_LEN 0x80
#define BPF_OP(c) ((c)&0xf0)
#define BPF_SRC(c) ((c)&0x08)
#define BPF_K 0x00
#define BPF_X 0x08
#define BPF_RVAL(c) ((c)&0x18)
#define BPF_A 0x10
#define BPF_MISCOP(c) ((c)&0xf8)
#define BPF_TAX 0x00
#define BPF_TXA 0x80
#define BPF_MEMWORDS 16

/* Checked once, at install: every jump lands inside, every load is an
 * aligned word of seccomp_data, and every path ends in a return. */
static bool validate(const struct sock_filter *f, int len) {
    if (len < 1 || len > BPF_MAXINSNS)
        return false;
    for (int pc = 0; pc < len; pc++) {
        const struct sock_filter *i = &f[pc];
        uint16_t c = i->code;
        switch (BPF_CLASS(c)) {
        case BPF_LD:
        case BPF_LDX:
            if (BPF_SIZE(c) != BPF_W && !(BPF_CLASS(c) == BPF_LDX && BPF_MODE(c) == 0xa0))
                return false;
            switch (BPF_MODE(c)) {
            case BPF_ABS:
                if (BPF_CLASS(c) != BPF_LD || (i->k & 3) || i->k >= sizeof(struct seccomp_data))
                    return false;
                break;
            case BPF_IMM:
            case BPF_LEN:
                break;
            case BPF_MEM:
                if (i->k >= BPF_MEMWORDS)
                    return false;
                break;
            default:
                return false;
            }
            break;
        case BPF_ST:
        case BPF_STX:
            if (i->k >= BPF_MEMWORDS)
                return false;
            break;
        case BPF_ALU:
            switch (BPF_OP(c)) {
            case 0x00: case 0x10: case 0x20: case 0x40: case 0x50: case 0x60: case 0x70: case 0xa0: case 0x80:
                break;
            case 0x30: /* div */
            case 0x90: /* mod */
                if (BPF_SRC(c) == BPF_K && i->k == 0)
                    return false;
                break;
            default:
                return false;
            }
            break;
        case BPF_JMP:
            if (BPF_OP(c) == 0x00) { /* ja */
                if ((uint64_t)pc + 1 + i->k >= (uint64_t)len)
                    return false;
            } else {
                if (BPF_OP(c) > 0x40)
                    return false;
                if (pc + 1 + i->jt >= len || pc + 1 + i->jf >= len)
                    return false;
            }
            break;
        case BPF_RET:
            if (BPF_RVAL(c) != BPF_K && BPF_RVAL(c) != BPF_A)
                return false;
            break;
        case BPF_MISC:
            if (BPF_MISCOP(c) != BPF_TAX && BPF_MISCOP(c) != BPF_TXA)
                return false;
            break;
        }
    }
    return BPF_CLASS(f[len - 1].code) == BPF_RET;
}

static uint32_t run(const struct seccomp_filter *flt, const struct seccomp_data *d) {
    uint32_t A = 0, X = 0, M[BPF_MEMWORDS] = {0};
    const uint8_t *data = (const uint8_t *)d;
    for (int pc = 0; pc < flt->len; pc++) {
        const struct sock_filter *i = &flt->insns[pc];
        uint16_t c = i->code;
        uint32_t src = BPF_SRC(c) == BPF_X ? X : i->k;
        switch (BPF_CLASS(c)) {
        case BPF_LD:
            switch (BPF_MODE(c)) {
            case BPF_ABS: memcpy(&A, data + i->k, 4); break;
            case BPF_IMM: A = i->k; break;
            case BPF_LEN: A = sizeof *d; break;
            case BPF_MEM: A = M[i->k]; break;
            }
            break;
        case BPF_LDX:
            switch (BPF_MODE(c)) {
            case BPF_IMM: X = i->k; break;
            case BPF_LEN: X = sizeof *d; break;
            case BPF_MEM: X = M[i->k]; break;
            }
            break;
        case BPF_ST: M[i->k] = A; break;
        case BPF_STX: M[i->k] = X; break;
        case BPF_ALU:
            switch (BPF_OP(c)) {
            case 0x00: A += src; break;
            case 0x10: A -= src; break;
            case 0x20: A *= src; break;
            case 0x30: if (!src) return SECCOMP_RET_KILL_THREAD; A /= src; break;
            case 0x40: A |= src; break;
            case 0x50: A &= src; break;
            case 0x60: A = src >= 32 ? 0 : A << src; break;
            case 0x70: A = src >= 32 ? 0 : A >> src; break;
            case 0x80: A = -A; break;
            case 0x90: if (!src) return SECCOMP_RET_KILL_THREAD; A %= src; break;
            case 0xa0: A ^= src; break;
            }
            break;
        case BPF_JMP:
            switch (BPF_OP(c)) {
            case 0x00: pc += (int)i->k; break;
            case 0x10: pc += A == src ? i->jt : i->jf; break;
            case 0x20: pc += A > src ? i->jt : i->jf; break;
            case 0x30: pc += A >= src ? i->jt : i->jf; break;
            case 0x40: pc += (A & src) ? i->jt : i->jf; break;
            }
            break;
        case BPF_RET: return BPF_RVAL(c) == BPF_A ? A : i->k;
        case BPF_MISC:
            if (BPF_MISCOP(c) == BPF_TAX)
                X = A;
            else
                A = X;
            break;
        }
    }
    return SECCOMP_RET_KILL_THREAD; /* not reached: validate() wants a return at the end */
}

static void filter_put(struct seccomp_filter *f) {
    while (f && __atomic_sub_fetch(&f->refcount, 1, __ATOMIC_ACQ_REL) == 0) {
        struct seccomp_filter *prev = f->prev;
        kfree(f);
        f = prev;
    }
}

int seccomp_mode(void) { return proc_current()->seccomp_mode; }

void seccomp_fork(struct process *parent, struct process *child) {
    child->no_new_privs = parent->no_new_privs;
    child->seccomp_mode = parent->seccomp_mode;
    child->seccomp = parent->seccomp;
    if (child->seccomp)
        __atomic_add_fetch(&child->seccomp->refcount, 1, __ATOMIC_RELAXED);
}

void seccomp_release(struct process *p) {
    filter_put(p->seccomp);
    p->seccomp = NULL;
}

int64_t seccomp_set(uint64_t mode, uint64_t flags, uint64_t uargs) {
    struct process *p = proc_current();
    if (mode == SECCOMP_SET_MODE_STRICT) {
        if (flags || uargs)
            return -EINVAL;
        if (p->seccomp_mode == 2)
            return -EINVAL;
        p->seccomp_mode = 1;
        return 0;
    }
    if (mode != SECCOMP_SET_MODE_FILTER)
        return -EINVAL;
    if (flags & ~(uint64_t)(SECCOMP_FILTER_FLAG_TSYNC | SECCOMP_FILTER_FLAG_LOG | SECCOMP_FILTER_FLAG_SPEC_ALLOW |
                            SECCOMP_FILTER_FLAG_TSYNC_ESRCH))
        return -EINVAL; /* NEW_LISTENER: user notification is not supported */
    if (p->seccomp_mode == 1)
        return -EINVAL;
    if (!p->no_new_privs && !ns_capable())
        return -EACCES;
    struct sock_fprog prog;
    if (copy_from_user(&prog, uargs, sizeof prog) < 0)
        return -EFAULT;
    if (prog.len < 1 || prog.len > BPF_MAXINSNS)
        return -EINVAL;
    struct seccomp_filter *f = kmalloc(sizeof *f + sizeof(struct sock_filter) * prog.len);
    if (!f)
        return -ENOMEM;
    if (copy_from_user(f->insns, prog.filter, sizeof(struct sock_filter) * prog.len) < 0) {
        kfree(f);
        return -EFAULT;
    }
    f->len = prog.len;
    f->refcount = 1;
    f->log = flags & SECCOMP_FILTER_FLAG_LOG;
    if (!validate(f->insns, f->len)) {
        kfree(f);
        return -EINVAL;
    }
    uint64_t lf = proc_table_lock();
    f->prev = p->seccomp;
    p->seccomp = f;
    p->seccomp_mode = 2;
    proc_table_unlock(lf);
    return 0;
}

SYSCALL(seccomp) {
    UNUSED_ARGS();
    switch (a1) {
    case SECCOMP_SET_MODE_STRICT:
    case SECCOMP_SET_MODE_FILTER:
        return seccomp_set(a1, a2, a3);
    case SECCOMP_GET_ACTION_AVAIL: {
        uint32_t action;
        if (a2 || copy_from_user(&action, a3, sizeof action) < 0)
            return a2 ? -EINVAL : -EFAULT;
        switch (action) {
        case SECCOMP_RET_KILL_PROCESS: case SECCOMP_RET_KILL_THREAD: case SECCOMP_RET_TRAP:
        case SECCOMP_RET_ERRNO: case SECCOMP_RET_TRACE: case SECCOMP_RET_LOG: case SECCOMP_RET_ALLOW:
            return 0;
        }
        return -EOPNOTSUPP;
    }
    case SECCOMP_GET_NOTIF_SIZES:
        return -EOPNOTSUPP;
    }
    return -EINVAL;
}

/* Before system call `nr` runs. Returns true to run it; otherwise `*ret`
 * is what the caller sees in rax (a SIGSYS may be on its way). */
bool seccomp_check(struct regs *r, uint64_t nr, int64_t *ret) {
    struct thread *t = thread_current();
    struct process *p = t->user->proc;
    if (p->seccomp_mode == 1) { /* strict: read, write, exit, sigreturn */
        if (nr == NR_read || nr == NR_write || nr == NR_exit || nr == NR_rt_sigreturn)
            return true;
        proc_exit(SIGKILL);
    }
    struct seccomp_data d = {.nr = (int32_t)nr, .arch = AUDIT_ARCH, .instruction_pointer = regs_ip(r)};
    for (int i = 0; i < 6; i++)
        d.args[i] = regs_syscall_arg(r, i);
    uint32_t result = SECCOMP_RET_ALLOW;
    for (struct seccomp_filter *f = p->seccomp; f; f = f->prev) {
        uint32_t cur = run(f, &d);
        if ((int32_t)(cur & SECCOMP_RET_ACTION_FULL) < (int32_t)(result & SECCOMP_RET_ACTION_FULL))
            result = cur;
    }
    uint32_t action = result & SECCOMP_RET_ACTION_FULL, data = result & SECCOMP_RET_DATA;
    switch (action) {
    case SECCOMP_RET_ALLOW:
    case SECCOMP_RET_LOG:
        return true;
    case SECCOMP_RET_ERRNO:
        *ret = -(int64_t)(data > 4095 ? 4095 : data);
        return false;
    case SECCOMP_RET_TRAP: {
        /* The handler sees the call as it was made: rax holds its number,
         * and siginfo says which call, from where. */
        struct user_thread *ut = t->user;
        ut->sigsys_nr = (int)nr;
        ut->sigsys_ip = regs_ip(r);
        ut->sigsys_errno = (int)data;
        ut->sigsys_pending = true;
        ut->sigmask &= ~sigbit(SIGSYS); /* forced, as Linux does */
        __atomic_or_fetch(&ut->pending_signals, sigbit(SIGSYS), __ATOMIC_ACQ_REL);
        *ret = (int64_t)nr;
        return false;
    }
    case SECCOMP_RET_TRACE:
    case SECCOMP_RET_USER_NOTIF:
        *ret = -ENOSYS; /* no tracer, no listener */
        return false;
    default: /* KILL_THREAD, KILL_PROCESS, and anything unknown */
        kprintf("seccomp: pid %d (%s) killed for system call %lu\n", p->pid, p->name, nr);
        proc_exit(SIGSYS);
    }
    return false;
}
