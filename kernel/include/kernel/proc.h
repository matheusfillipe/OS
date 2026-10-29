#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <kernel/arch.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/ns.h>
#include <kernel/vfs.h>
#include <linux/abi.h>

struct vnode;
struct file;

/* Wait queue: threads block here until a condition they re-check holds.
 * Usage:
 *     while (!cond) waitq_wait(&wq);           (or with a timeout/interrupt)
 * Wakeups may be spurious; always re-check the condition. */
struct waitq_entry {
    struct thread *t;
    struct waitq_entry *next;
};
struct waitq {
    spinlock_t lock;
    struct waitq_entry *head;
    uint32_t seq; /* bumped by every wake: lets epoll see "something happened" (EPOLLET) */
};
void waitq_init(struct waitq *wq);
/* Registers the current thread, re-checks `cond(arg)`, then blocks if it is
 * still false. Returns -EINTR if a signal is pending, 0 otherwise. */
int waitq_wait_cond(struct waitq *wq, bool (*cond)(void *), void *arg);
void waitq_wake_all(struct waitq *wq);

/* User address space. */
#define USER_TOP        0x0000800000000000ULL
#define USER_STACK_TOP  0x00007ffffff00000ULL
#define USER_STACK_SIZE (8ULL << 20)
#define USER_MMAP_BASE  0x0000700000000000ULL
#define USER_INTERP_BASE 0x00007f0000000000ULL

#define VMA_ANON     1
#define VMA_GROWSDOWN 2
#define VMA_PHYS      4 /* maps device memory: pages are not owned or freed */
#define VMA_SHARED    8 /* maps `obj`'s pages (MAP_SHARED): each PTE holds a page reference */

struct pageobj;

/* The file a mapping came from, as /proc/<pid>/maps shows it. Shared (by
 * reference) between the VMAs one mapping is split into. */
struct vma_name {
    int refcount;
    uint64_t ino;
    char path[];
};
struct vma_name *vma_name_create(const char *path, uint64_t ino);
/* For the file at `path` (relative to `cwd`), by its resolved absolute path. */
struct vma_name *vma_name_lookup(const struct path *cwd, const char *path);
/* For a file being mapped: the path it was opened by, or NULL. */
struct vma_name *vma_name_for_file(struct file *f);
void vma_name_put(struct vma_name *n);

struct vma {
    uint64_t start, end;
    uint32_t prot;
    uint32_t flags;
    struct pageobj *obj; /* VMA_SHARED: referenced backing object */
    uint64_t pgoff;      /* VMA_SHARED: page index in `obj` of `start`; file maps: file page, for maps */
    struct vma_name *name; /* referenced, or NULL (anonymous) */
    struct vma *next;
};

struct mm {
    uint64_t *pgd;
    struct vma *vmas; /* sorted by start */
    uint64_t brk_start, brk;
    uint64_t mmap_hint;
    int refcount;
    spinlock_t lock;
};

struct mm *mm_create(void);
struct mm *mm_clone(struct mm *src);
void mm_put(struct mm *mm);
void mm_activate(struct mm *mm);
/* Map [start, end) with prot; pages are allocated on first touch. */
int mm_map(struct mm *mm, uint64_t start, uint64_t end, uint32_t prot, uint32_t flags);
/* Maps physical device memory at [start, end), write-combining. */
int mm_map_phys(struct mm *mm, uint64_t start, uint64_t end, uint32_t prot, uint64_t phys);
int mm_unmap(struct mm *mm, uint64_t start, uint64_t end);
int mm_protect(struct mm *mm, uint64_t start, uint64_t end, uint32_t prot);
uint64_t mm_find_free(struct mm *mm, uint64_t len, uint64_t hint);
/* Finds a free range (near `hint` if possible) and maps it, atomically.
 * Returns its address, or 0 if there is no room. */
uint64_t mm_map_free(struct mm *mm, uint64_t len, uint64_t hint, uint32_t prot, uint32_t flags);
struct vma *mm_find_vma(struct mm *mm, uint64_t addr);
/* Maps `len` bytes of `obj` from page `pgoff` on, shared: at `addr` exactly
 * (replacing what is there) if `fixed`, else at a free range near `addr`.
 * The mapping takes its own reference on `obj`. Returns the address or a
 * negative errno. */
int64_t mm_map_shared(struct mm *mm, uint64_t addr, uint64_t len, uint32_t prot, bool fixed, struct pageobj *obj,
                      uint64_t pgoff);
/* mremap for anonymous and shared mappings: grows in place when the space
 * after it is free, else moves the page-table entries to a new range if
 * `may_move`. Returns the new address or a negative errno. */
int64_t mm_remap(struct mm *mm, uint64_t old_addr, uint64_t old_len, uint64_t new_len, bool may_move);
/* madvise(MADV_DONTNEED): anonymous private pages in the range are dropped
 * (they read as zeros on next touch); shared ones are only unmapped (the
 * next touch maps the object's page again). Other mappings are left alone. */
int mm_dontneed(struct mm *mm, uint64_t start, uint64_t end);
/* Copy into another address space through the direct map (used by exec
 * before the new address space is live). Allocates pages as needed. */
int mm_copy_out(struct mm *mm, uint64_t dst, const void *src, size_t len);
int mm_zero(struct mm *mm, uint64_t dst, size_t len);
/* Handles a page fault; returns true if resolved. */
bool mm_handle_fault(struct mm *mm, uint64_t addr, bool write, bool exec);
/* Labels the VMAs lying wholly inside [start, end) with `name` (a new
 * reference each) and, for private ones, the file page of their start. */
void mm_set_name(struct mm *mm, uint64_t start, uint64_t end, struct vma_name *name, uint64_t pgoff);
/* Pages actually present in the page tables (/proc's RSS). */
uint64_t mm_rss_pages(struct mm *mm);

/* Loads the kernel's own page tables on this CPU (no user mm active). */
void mm_activate_kernel(void);
/* Makes every other CPU that has `mm` loaded flush its TLB, and waits until
 * they have. Call after changing or removing PTEs, before freeing pages. */
void tlb_shootdown(struct mm *mm);

/* Access to the current process's memory. A fault on an unmapped or
 * forbidden page returns -EFAULT (through the exception fixup table), so
 * another thread unmapping memory concurrently cannot crash the kernel. */
int copy_from_user(void *dst, uint64_t src, size_t len);
int copy_to_user(uint64_t dst, const void *src, size_t len);
/* Copies a NUL-terminated string; returns length or negative errno. */
int64_t strncpy_from_user(char *dst, uint64_t src, size_t size);
/* Atomic compare-and-exchange on a user word: *cur receives the old value.
 * Returns 0, or -EFAULT. */
int futex_cmpxchg_user(uint64_t uaddr, uint32_t expected, uint32_t desired, uint32_t *cur);
/* If `rip` is a user-access instruction with a fixup, returns where to
 * resume; 0 otherwise. */
uint64_t exception_fixup(uint64_t rip);

#define MAX_FDS 256

struct fdtable {
    struct file *fd[MAX_FDS];
    uint8_t cloexec[MAX_FDS];
    int refcount;
    spinlock_t lock;
};

enum proc_state { PROC_ALIVE, PROC_ZOMBIE };

struct user_thread;

/* What /proc shows of the running image: the executable's absolute path
 * (resolved at execve) and copies of argv and envp, NUL-separated. Shared
 * by reference with forked children until they exec. */
struct exec_info {
    int refcount;
    char *exe;
    char *cmdline, *environ;
    size_t cmdline_len, environ_len;
};
struct exec_info *exec_info_create(const struct path *cwd, const char *path, char *const argv[], char *const envp[]);
void exec_info_put(struct exec_info *ei);

/* A process is a Linux thread group: every thread in it shares the address
 * space, descriptors, cwd and signal handlers kept here. pid is the thread
 * group id; each thread has its own tid (the first thread's tid == pid). */
struct process {
    int pid, pgid, sid;
    int ctty; /* controlling terminal: 0 console, -1 none, n > 0 pty n-1 */
    enum proc_state state;
    int exit_status; /* wait4 encoding */
    struct process *parent;
    struct process *children, *sibling;
    struct user_thread *threads; /* live threads, under proc_lock */
    int nthreads;
    bool group_exit;          /* exit_group or a fatal signal: all threads die */
    struct waitq thread_wait; /* execve waits here for the other threads to go */
    struct mm *mm;
    struct fdtable *fds;
    /* Where its paths start: its root (chroot, pivot_root) and working
     * directory, and the mount namespace both live in. */
    struct path root, cwd;
    struct mntns *mntns;
    struct ns_set ns;     /* its other namespaces (kernel/ns.h) */
    bool child_subreaper; /* prctl(PR_SET_CHILD_SUBREAPER): orphans below it come to it */
    int pdeath_sig;       /* PR_SET_PDEATHSIG: sent to it when its parent ends */
    bool not_dumpable, keep_caps, no_new_privs;
    struct seccomp_filter *seccomp; /* the newest filter; each holds the one before (referenced) */
    int seccomp_mode;     /* 0 off, 1 strict, 2 filters */
    uint32_t umask;
    uint32_t uid, gid, euid, egid;
    /* Capabilities, as the bits NOT held (so a process made by the kernel
     * has them all); inheritable as the bits held. See kernel/linux/sys_proc.c. */
    uint64_t cap_eff_off, cap_perm_off, cap_inh;
    struct linux_sigaction sigactions[NSIG];
    uint64_t pending_signals; /* process-wide: any thread not blocking it takes it */
    struct waitq child_wait; /* parent waits here for children */
    struct waitq vfork_wait; /* vfork parent waits for exec/exit */
    volatile bool vfork_done;
    char name[16];
    struct process *next_all;
    /* /proc reads mm, fds, cwd and exec of other processes: those pointers
     * are swapped under the process table lock (proc_table_lock). */
    struct exec_info *exec;
    uint64_t start_ns;               /* creation, monotonic */
    uint64_t dead_utime, dead_stime; /* timer ticks of threads that have exited */
};

/* Per-thread user state (lives in struct thread via `user`). */
struct user_thread {
    struct process *proc;
    struct thread *thread;
    struct user_thread *next_in_proc; /* process thread list, under proc_lock */
    int tid;
    volatile bool killed; /* must exit at the next return to user (group exit, execve) */
    bool exiting;         /* past the point of no return in thread exit: no mm */
    void *fpu;     /* arch floating-point state */
    uint64_t tls;
    uint64_t clear_child_tid;
    uint64_t sigmask;
    uint64_t pending_signals; /* directed at this thread (tgkill, faults) */
    /* A temporary mask (rt_sigsuspend, ppoll, pselect6, epoll_pwait) is in
     * sigmask; this is the one to go back to, either through the signal
     * frame of the handler that interrupted the call or at syscall exit. */
    uint64_t saved_sigmask;
    bool restore_sigmask;
    uint64_t utime, stime; /* timer ticks that found it in user / kernel mode */
    /* The system call it is in (-1: running its own code), for finding hangs
     * (proc_dump_threads). */
    int64_t sc_nr;
    uint64_t sc_a1, sc_a2, sc_a3;
    /* the last system calls it made (debug: printed by the hang dump for traced programs) */
    struct { int16_t nr; int64_t ret; uint64_t a1, a2, a3; } sc_hist[16];
    uint8_t sc_hist_n;
    /* seccomp SECCOMP_RET_TRAP: what the SIGSYS it raised reports */
    bool sigsys_pending;
    int sigsys_nr, sigsys_errno;
    uint64_t sigsys_ip;
};

/* seccomp (proc/seccomp.c) */
int seccomp_mode(void);
int64_t seccomp_set(uint64_t mode, uint64_t flags, uint64_t uargs);
bool seccomp_check(struct regs *r, uint64_t nr, int64_t *ret);
void seccomp_fork(struct process *parent, struct process *child);
void seccomp_release(struct process *p);
struct seccomp_filter;

/* pivot_root: every process of `ns` whose root or cwd was `old` moves to `new`. */
void proc_move_roots(struct mntns *ns, const struct path *old, const struct path *new);
/* The first mount namespace (referenced), for the kernel and init. */
struct mntns *proc_first_mntns(void);

/* Every user thread with the call it is blocked in, to the serial log. */
void proc_dump_threads(void);

/* Every signal set in the kernel (thread masks, pending sets, sa_mask,
 * signalfd masks) has the layout of Linux's sigset_t: bit n-1 is signal n,
 * so user sets are stored and returned as they are. */
static inline uint64_t sigbit(int sig) { return 1ULL << (sig - 1); }
#define SIG_UNBLOCKABLE (sigbit(SIGKILL) | sigbit(SIGSTOP))

struct process *proc_current(void);
struct process *proc_find(int pid);
/* Creates PID 1 and runs `path` as a user program in it. */
void proc_start_init(const char *path);
/* Ends the whole thread group (exit_group, fatal signals). */
__attribute__((noreturn)) void proc_exit(int status);
/* Ends only the calling thread; the last one out makes the process a zombie. */
__attribute__((noreturn)) void thread_exit_user(int status);
void proc_kill(struct process *p, int sig);
/* Sends to one thread (tkill/tgkill). Returns -ESRCH if there is no such
 * thread, or if tgid > 0 and the thread is not in that group. */
int proc_kill_thread(int tgid, int tid, int sig);
/* True if the current thread should abandon a blocking wait with -EINTR. */
bool signal_pending_current(void);
/* Gives the calling thread sole ownership of its process (execve): the other
 * threads are told to exit and waited for. Returns 0 or -EINTR. */
int proc_kill_other_threads(void);
/* Attaches a new thread to `p`; if p is exiting, the thread starts killed. */
void proc_add_thread(struct process *p, struct user_thread *ut);
int proc_alloc_tid(void);

/* For /proc. The table lock keeps processes from being reaped and their
 * mm/fds/cwd/exec pointers from changing; it is a spinlock (IRQs off). */
uint64_t proc_table_lock(void);
void proc_table_unlock(uint64_t flags);
/* Calls fn(p, arg) with the table locked; -ESRCH if there is no such process. */
int proc_inspect(int pid, void (*fn)(struct process *p, void *arg), void *arg);
/* Fills `pids` with up to `max` process ids (zombies included); returns the count. */
int proc_list_pids(int *pids, int max);
int proc_last_pid(void);
/* Timer interrupt: charges the tick to the running thread and CPU. */
void proc_account_tick(struct regs *r);

/* futex: wakes up to `n` waiters on (mm, uaddr) whose bitset intersects. */
int futex_wake(struct mm *mm, uint64_t uaddr, int n, uint32_t bitset);

int do_execve(const char *path, char *const argv[], char *const envp[], struct regs *frame);

/* Syscall frame of the current thread (top of its kernel stack). */
struct regs *current_user_frame(void);
__attribute__((noreturn)) void return_to_user(struct regs *frame);

/* FPU / SSE / AVX state */
void fpu_init_cpu(void);
void *fpu_alloc(void);
void fpu_free(void *area);
void fpu_save(void *area);
void fpu_restore(void *area);
void fpu_copy(void *dst, const void *src);
void fpu_reset(void *area);
void fpu_capture_clean_state(void);

/* System call entry */
void syscall_init_cpu(void);
void syscall_dispatch(struct regs *r);

/* Time */
void time_init(void);
uint64_t time_monotonic_ns(void);
int64_t time_realtime_s(void);
uint64_t time_realtime_ns(void);
