#include <asm/regs.h>
#include <asm/syscall_table.h>
#include <kernel/lib.h>
#include <kernel/proc.h>
#include <linux/syscall.h>

#define DECLARE(nr, name) SYSCALL(name);
SYSCALLS(DECLARE)

#define NR_SYSCALLS 512
#define ENTRY(nr, name) [nr] = sys_##name,
static const syscall_fn table[NR_SYSCALLS] = {SYSCALLS(ENTRY)};

static const char *const names[NR_SYSCALLS] = {
#define NAME(nr, name) [nr] = #name,
    SYSCALLS(NAME)
};

static bool takes_path_arg0(const char *name) {
    return !strcmp(name, "open") || !strcmp(name, "stat") || !strcmp(name, "lstat") || !strcmp(name, "access") ||
           !strcmp(name, "readlink");
}

static bool takes_path_arg1(const char *name) {
    return !strcmp(name, "openat") || !strcmp(name, "newfstatat") || !strcmp(name, "readlinkat") ||
           !strcmp(name, "faccessat") || !strcmp(name, "faccessat2");
}

static uint8_t warned[NR_SYSCALLS / 8];

void signal_deliver(struct regs *r);
static bool strace;

void syscall_set_trace(bool on);
void syscall_set_trace(bool on) { strace = on; }

/* fw_cfg opt/os/strace=NAME: only the failing calls of programs called
 * NAME, which is what finding a crash usually needs, without the flood. */
static char strace_name[16];
void syscall_trace_failures(const char *name);
void syscall_trace_failures(const char *name) { ksnprintf(strace_name, sizeof strace_name, "%s", name); }
bool syscall_traced(const char *name);
bool syscall_traced(const char *name) { return strace_name[0] && !strcmp(name, strace_name); }

void syscall_dispatch(struct regs *r) {
    uint64_t nr = regs_syscall_nr(r);
    uint64_t a0 = regs_syscall_arg(r, 0), a1 = regs_syscall_arg(r, 1), a2 = regs_syscall_arg(r, 2);
    int64_t ret;
    struct thread *self = thread_current();
    struct user_thread *ut = self ? self->user : NULL;
    if (ut)
        ut->sc_nr = (int64_t)nr, ut->sc_a1 = a0, ut->sc_a2 = a1, ut->sc_a3 = a2;
    if (ut && ut->proc->seccomp_mode && !seccomp_check(r, nr, &ret)) {
        /* refused by a seccomp filter: ret is the answer */
    } else if (nr < NR_SYSCALLS && table[nr]) {
        ret = table[nr](a0, a1, a2, regs_syscall_arg(r, 3), regs_syscall_arg(r, 4), regs_syscall_arg(r, 5), r);
        if (strace)
            kprintf("[%d] %s(%lx, %lx, %lx) = %ld\n", proc_current()->pid, names[nr], a0, a1, a2, ret);
        else if (strace_name[0] && ret < 0 && ret != -EAGAIN && !strcmp(proc_current()->name, strace_name)) {
            /* the path, for the calls that take one */
            uint64_t up = takes_path_arg0(names[nr]) ? a0 : takes_path_arg1(names[nr]) ? a1 : 0;
            char path[96] = "";
            if (up && strncpy_from_user(path, up, sizeof path) < 0)
                path[0] = 0;
            path[sizeof path - 1] = 0;
            kprintf("[%d/%d] %s(%lx, %lx, %lx) = %ld %s\n", proc_current()->pid, ut ? ut->tid : 0, names[nr], a0, a1,
                    a2, ret, path);
        }
    } else {
        /* rseq is an optional glibc fast path; ENOSYS is the expected answer. */
        if (nr < NR_SYSCALLS && nr != NR_rseq && !(warned[nr / 8] & (1 << (nr % 8)))) {
            warned[nr / 8] |= (uint8_t)(1 << (nr % 8));
            kprintf("syscall %lu not implemented (pid %d, %s)\n", nr, proc_current()->pid, proc_current()->name);
        }
        ret = -ENOSYS;
    }
    if (ut) { /* recent history, for the hang dump */
        int i = ut->sc_hist_n++ % 16;
        ut->sc_hist[i].nr = (int16_t)nr, ut->sc_hist[i].ret = ret;
        ut->sc_hist[i].a1 = ut->sc_a1, ut->sc_hist[i].a2 = ut->sc_a2, ut->sc_hist[i].a3 = ut->sc_a3;
    }
    /* execve and rt_sigreturn rewrite the whole frame, rax included. */
    if (nr != NR_rt_sigreturn && !(nr == NR_execve && ret == 0))
        regs_set_return(r, (uint64_t)ret);
    self = thread_current();
    if (self && self->user)
        self->user->sc_nr = -1;
    signal_deliver(r);
}

const char *syscall_name(int64_t nr);
const char *syscall_name(int64_t nr) {
    return nr >= 0 && nr < NR_SYSCALLS && names[nr] ? names[nr] : "?";
}
