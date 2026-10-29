#include <kernel/arch.h>
#include <kernel/blkdev.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/net.h>
#include <kernel/proc.h>
#include <kernel/vfs.h>
#include <linux/abi.h>
#include "synth.h"

/* /proc: processes and system information in the Linux format, generated
 * on every read. Programs depend on it in ways that are easy to miss:
 * glibc's dynamic loader resolves $ORIGIN through /proc/self/exe,
 * pthread_getattr_np reads the [stack] line of /proc/self/maps, ttyname
 * reads /proc/self/fd/N, ps reads /proc/<pid>/stat, and runtimes size their
 * thread pools from /proc/cpuinfo and /sys. */

#define PATH_MAX 4096
#define USER_HZ  100 /* what clock-tick fields count in (AT_CLKTCK) */

void fdtable_put(struct fdtable *t);
void random_bytes(void *buf, size_t len);

static uint64_t ticks_to_clk(uint64_t ticks) { return ticks * USER_HZ / HZ; }

/* Zero-padded octal (the kernel's printf has no %o). */
static void octal(char *out, uint32_t v, int width) {
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + (v & 7));
        v >>= 3;
    } while (v);
    while (n < width)
        tmp[n++] = '0';
    for (int i = 0; i < n; i++)
        out[i] = tmp[n - 1 - i];
    out[n] = 0;
}

/* ---- paths ---- */

/* Lexical normalization of an absolute path in place: "//", "." and ".."
 * collapse; symlinks are not looked at. */
static void normalize(char *p) {
    char *out = p, *in = p;
    while (*in) {
        while (*in == '/')
            in++;
        if (!*in)
            break;
        char *seg = in;
        while (*in && *in != '/')
            in++;
        size_t n = (size_t)(in - seg);
        if (n == 1 && seg[0] == '.')
            continue;
        if (n == 2 && seg[0] == '.' && seg[1] == '.') {
            while (out > p && *--out != '/')
                ;
            continue;
        }
        *out++ = '/';
        memmove(out, seg, n);
        out += n;
    }
    if (out == p)
        *out++ = '/';
    *out = 0;
}

/* The absolute name of `base` (NULL: the cwd), as the caller sees it. */
static int base_name(const struct path *base, char *out, size_t size) {
    if (base && base->v)
        return path_name_here(base, out, size);
    struct path cwd;
    fs_context(NULL, &cwd);
    int r = path_name_here(&cwd, out, size);
    path_put(&cwd);
    return r;
}

/* `path` made absolute (from `base` if relative) and normalized. */
static int lexical_abs(const struct path *base, const char *path, char *out, size_t size) {
    size_t pl = strlen(path);
    if (path[0] == '/') {
        if (pl + 1 > size)
            return -ENAMETOOLONG;
        memcpy(out, path, pl + 1);
    } else {
        int n = base_name(base, out, size);
        if (n < 0)
            return n;
        size_t l = (size_t)n;
        if (l == 1)
            l = 0; /* "/" + "/" + path */
        if (l + 1 + pl + 1 > size)
            return -ENAMETOOLONG;
        out[l] = '/';
        memcpy(out + l + 1, path, pl + 1);
    }
    normalize(out);
    return (int)strlen(out);
}

/* Resolves every component, following symlinks as the kernel's own lookup
 * would. A "magic" link (/proc/<pid>/fd/N, cwd, exe) is followed by its
 * text when `magic`; otherwise only when it names a directory (by that
 * directory's path). /proc/<pid>/fd/N text is itself made with this
 * function, so turning `magic` off there bounds the recursion. */
static int realpath_impl(const struct path *base, const char *path, char *out, size_t size, bool magic) {
    char *res = kmalloc(PATH_MAX), *rem = kmalloc(PATH_MAX), *tmp = kmalloc(PATH_MAX);
    int r = 0, links = 0;
    if (!res || !rem || !tmp) {
        r = -ENOMEM;
        goto done;
    }
    if (strlen(path) >= PATH_MAX) {
        r = -ENAMETOOLONG;
        goto done;
    }
    if (path[0] == '/') {
        ksnprintf(res, PATH_MAX, "/");
    } else if ((r = base_name(base, res, PATH_MAX)) < 0) {
        goto done;
    }
    ksnprintf(rem, PATH_MAX, "%s", path);
    r = 0;
    char *p = rem;
    for (;;) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        char *seg = p;
        while (*p && *p != '/')
            p++;
        size_t n = (size_t)(p - seg);
        if (n == 1 && seg[0] == '.')
            continue;
        size_t rl = strlen(res);
        if (n == 2 && seg[0] == '.' && seg[1] == '.') {
            while (rl > 1 && res[rl - 1] != '/')
                rl--;
            if (rl > 1)
                rl--;
            res[rl] = 0;
            continue;
        }
        if (rl + 1 + n + 1 > PATH_MAX) {
            r = -ENAMETOOLONG;
            goto done;
        }
        size_t cl = rl;
        if (rl > 1)
            res[cl++] = '/';
        memcpy(res + cl, seg, n);
        res[cl + n] = 0;
        struct vnode *v;
        if ((r = vfs_lookup(NULL, res, 0, &v)) < 0)
            goto done;
        if (v->type != VN_LNK) {
            vnode_put(v);
            continue;
        }
        res[rl] = 0; /* the link's directory, for a relative target */
        if (++links > 40) {
            vnode_put(v);
            r = -ELOOP;
            goto done;
        }
        int tl;
        if (v->ops->follow && !magic) {
            struct path tp = {0};
            r = v->ops->follow(v, &tp);
            vnode_put(v);
            if (r < 0)
                goto done;
            if (!tp.mnt) {
                struct vnode *t = tp.v;
                path_for_vnode(t, &tp);
                vnode_put(t);
            }
            r = tp.v->type == VN_DIR ? path_name_here(&tp, res, PATH_MAX) : -ENOENT;
            path_put(&tp);
            if (r < 0)
                goto done;
            r = 0;
            continue;
        }
        tl = v->ops->readlink ? v->ops->readlink(v, tmp, PATH_MAX - 1) : -EINVAL;
        vnode_put(v);
        if (tl < 0) {
            r = tl;
            goto done;
        }
        tmp[tl] = 0;
        /* The rest of the path goes after the link's target. */
        size_t restl = strlen(p);
        if ((size_t)tl + 1 + restl + 1 > PATH_MAX) {
            r = -ENAMETOOLONG;
            goto done;
        }
        memmove(rem + tl + 1, p, restl + 1);
        memcpy(rem, tmp, (size_t)tl);
        rem[tl] = '/';
        p = rem;
        if (tmp[0] == '/')
            ksnprintf(res, PATH_MAX, "/");
    }
    if (strlen(res) + 1 > size)
        r = -ENAMETOOLONG;
    else
        r = ksnprintf(out, size, "%s", res);
done:
    kfree(res);
    kfree(rem);
    kfree(tmp);
    return r;
}

int vfs_realpath(const struct path *base, const char *path, char *out, size_t size) {
    return realpath_impl(base, path, out, size, true);
}

int file_describe(struct file *f, char *out, size_t size) {
    struct vnode *v = f->vnode;
    if (f->path) {
        const struct path *pb = f->path_base.v ? &f->path_base : NULL;
        int r = realpath_impl(pb, f->path, out, size, false);
        if (r >= 0)
            return r;
        /* Gone (unlinked, or its terminal closed): Linux's wording. */
        r = lexical_abs(pb, f->path, out, size);
        if (r >= 0 && (size_t)r + 11 < size)
            return r + ksnprintf(out + r, size - (size_t)r, " (deleted)");
    }
    switch (v->type) {
    case VN_FIFO:
        return ksnprintf(out, size, "pipe:[%lu]", v->ino);
    case VN_SOCK:
        return ksnprintf(out, size, "socket:[%lu]", v->ino);
    case VN_ANON:
        return ksnprintf(out, size, "anon_inode:[os]");
    case VN_DIR: {
        struct path p;
        if (f->mnt) {
            p.mnt = mount_get(f->mnt);
            p.v = v;
            vnode_ref(v);
        } else {
            path_for_vnode(v, &p);
        }
        int r = path_name_here(&p, out, size);
        path_put(&p);
        return r;
    }
    case VN_CHR:
        if ((v->rdev >> 8) == 136) /* a pty slave reached without a path (TIOCGPTPEER) */
            return ksnprintf(out, size, "/dev/pts/%u", v->rdev & 0xff);
        break;
    case VN_REG:
        return ksnprintf(out, size, "/memfd:os (deleted)");
    default:
        break;
    }
    return ksnprintf(out, size, "anon_inode:[%lu]", v->ino);
}

/* ---- what execve records ---- */

static size_t join_strings(char *dst, char *const v[], size_t cap) {
    size_t n = 0;
    for (int i = 0; v[i]; i++) {
        size_t l = strlen(v[i]) + 1;
        if (n + l > cap)
            break;
        if (dst)
            memcpy(dst + n, v[i], l);
        n += l;
    }
    return n;
}

#define EXEC_STRINGS_MAX (128u << 10) /* execve's own limit on argv+envp */

struct exec_info *exec_info_create(const struct path *cwd, const char *path, char *const argv[], char *const envp[]) {
    char *exe = kmalloc(PATH_MAX);
    if (!exe)
        return NULL;
    if (vfs_realpath(cwd, path, exe, PATH_MAX) < 0 && lexical_abs(cwd, path, exe, PATH_MAX) < 0)
        ksnprintf(exe, PATH_MAX, "%s", path);
    size_t el = strlen(exe) + 1;
    size_t al = join_strings(NULL, argv, EXEC_STRINGS_MAX), vl = join_strings(NULL, envp, EXEC_STRINGS_MAX);
    struct exec_info *ei = kmalloc(sizeof *ei + el + al + vl);
    if (ei) {
        char *d = (char *)(ei + 1);
        ei->refcount = 1;
        ei->exe = d;
        memcpy(d, exe, el);
        ei->cmdline = d + el;
        ei->cmdline_len = join_strings(ei->cmdline, argv, EXEC_STRINGS_MAX);
        ei->environ = ei->cmdline + ei->cmdline_len;
        ei->environ_len = join_strings(ei->environ, envp, EXEC_STRINGS_MAX);
    }
    kfree(exe);
    return ei;
}

void exec_info_put(struct exec_info *ei) {
    if (__atomic_sub_fetch(&ei->refcount, 1, __ATOMIC_ACQ_REL) == 0)
        kfree(ei);
}

struct vma_name *vma_name_lookup(const struct path *cwd, const char *path) {
    char *abs = kmalloc(PATH_MAX);
    if (!abs)
        return NULL;
    if (vfs_realpath(cwd, path, abs, PATH_MAX) < 0)
        ksnprintf(abs, PATH_MAX, "%s", path);
    uint64_t ino = 0;
    struct vnode *v;
    if (vfs_lookup(cwd, abs, LOOKUP_FOLLOW, &v) == 0) {
        ino = v->ino;
        vnode_put(v);
    }
    struct vma_name *n = vma_name_create(abs, ino);
    kfree(abs);
    return n;
}

/* mmap is frequent (ld.so maps each library several times), so the name
 * is the opening path made absolute, without resolving symlinks. */
struct vma_name *vma_name_for_file(struct file *f) {
    if (!f->path)
        return NULL;
    char *abs = kmalloc(PATH_MAX);
    if (!abs)
        return NULL;
    struct vma_name *n = lexical_abs(f->path_base.v ? &f->path_base : NULL, f->path, abs, PATH_MAX) >= 0 ? vma_name_create(abs, f->vnode->ino)
                                                                               : NULL;
    kfree(abs);
    return n;
}

/* ---- CPU time accounting (timer interrupt) ---- */

enum { CT_USER, CT_SYS, CT_IDLE, CT_N };
static uint64_t cpu_time[MAX_CPUS][CT_N];
static uint64_t busy_ticks;           /* non-idle ticks over all CPUs, for loadavg */
static uint64_t loadavg_fixed[3];     /* 11-bit fixed point, as Linux */

void proc_account_tick(struct regs *r) {
    struct cpu *c = this_cpu();
    struct thread *t = c->current;
    if (t == c->idle) {
        cpu_time[c->id][CT_IDLE]++;
    } else {
        __atomic_add_fetch(&busy_ticks, 1, __ATOMIC_RELAXED);
        if (regs_user_mode(r)) {
            cpu_time[c->id][CT_USER]++;
            if (t && t->user)
                t->user->utime++;
        } else {
            cpu_time[c->id][CT_SYS]++;
            if (t && t->user)
                t->user->stime++;
        }
    }
    /* Every 5 s: the average number of busy CPUs feeds the 1, 5 and 15
     * minute load averages (Linux counts runnable threads; we have no
     * cheap way to see the run queue from here). */
    if (c->id == 0 && global_ticks % (5 * HZ) == 0) {
        static const uint64_t expo[3] = {1884, 2014, 2037};
        uint64_t sample = __atomic_exchange_n(&busy_ticks, 0, __ATOMIC_RELAXED) * 2048 / (5 * HZ);
        for (int i = 0; i < 3; i++)
            loadavg_fixed[i] = (loadavg_fixed[i] * expo[i] + sample * (2048 - expo[i])) / 2048;
    }
}

/* ---- process snapshots ---- */

#define W_MM   1
#define W_FDS  2
#define W_CWD  4
#define W_EXEC 8

/* Everything a /proc file needs from one process, copied (or referenced)
 * under the process table lock. */
struct psnap {
    int want, want_tid;
    bool thread_found, zombie;
    int pid, tid, ppid, pgid, sid, ctty, nthreads, cpu;
    int gpid, gtid; /* global ids, for the NSpid chain */
    struct pidns *pidns; /* not referenced: only compared */
    char state;
    char comm[32];
    uint32_t uid, gid, euid, egid, umask;
    uint64_t utime, stime, start_ns;
    uint64_t sigpnd, shdpnd, sigblk, sigign, sigcgt;
    struct mm *mm;
    struct fdtable *fds;
    struct path cwd, root;
    struct mntns *mntns;
    struct exec_info *ei;
};

static char thread_state(struct user_thread *ut) {
    enum thread_state s = ut->thread->state;
    return s == THREAD_RUNNING || s == THREAD_READY ? 'R' : s == THREAD_DEAD ? 'X' : 'S';
}

static void snap_fn(struct process *p, void *arg) {
    struct psnap *s = arg;
    /* numbers and ids as the reader (this process) names them */
    s->gpid = p->pid;
    s->gtid = s->want_tid ? s->want_tid : p->pid;
    s->pid = pid_to_user(p->pid);
    s->tid = pid_to_user(s->gtid);
    s->ppid = p->parent ? pid_to_user(p->parent->pid) : 0;
    s->pgid = pid_to_user(p->pgid), s->sid = pid_to_user(p->sid), s->ctty = p->ctty;
    s->uid = uid_to_user(p->uid), s->gid = gid_to_user(p->gid), s->euid = uid_to_user(p->euid);
    s->egid = gid_to_user(p->egid), s->umask = p->umask;
    s->pidns = p->ns.pid;
    s->start_ns = p->start_ns;
    s->zombie = p->state == PROC_ZOMBIE;
    s->shdpnd = p->pending_signals;
    for (int i = 1; i < NSIG; i++) {
        if (p->sigactions[i].handler == SIG_IGN)
            s->sigign |= sigbit(i);
        else if (p->sigactions[i].handler != SIG_DFL)
            s->sigcgt |= sigbit(i);
    }
    ksnprintf(s->comm, sizeof s->comm, "%s", p->name);
    s->state = s->zombie ? 'Z' : 'S';
    uint64_t ut_sum = p->dead_utime, st_sum = p->dead_stime;
    for (struct user_thread *ut = p->threads; ut; ut = ut->next_in_proc) {
        s->nthreads++;
        ut_sum += ut->utime;
        st_sum += ut->stime;
        if (!s->want_tid && thread_state(ut) == 'R')
            s->state = 'R';
        if (ut->tid == s->gtid) {
            s->thread_found = true;
            s->sigblk = ut->sigmask;
            s->sigpnd = ut->pending_signals;
            s->cpu = ut->thread->last_cpu;
            if (s->want_tid) {
                s->state = thread_state(ut);
                s->utime = ut->utime;
                s->stime = ut->stime;
                ksnprintf(s->comm, sizeof s->comm, "%s", ut->thread->name);
            }
        }
    }
    if (!s->want_tid) {
        s->utime = ut_sum;
        s->stime = st_sum;
    }
    if ((s->want & W_MM) && p->mm) {
        s->mm = p->mm;
        __atomic_add_fetch(&s->mm->refcount, 1, __ATOMIC_RELAXED);
    }
    if ((s->want & W_FDS) && p->fds) {
        s->fds = p->fds;
        __atomic_add_fetch(&s->fds->refcount, 1, __ATOMIC_RELAXED);
    }
    if ((s->want & W_CWD) && p->cwd.v) {
        s->cwd = p->cwd;
        path_get(&s->cwd);
        s->root = p->root;
        path_get(&s->root);
        s->mntns = mntns_get(p->mntns);
    }
    if ((s->want & W_EXEC) && p->exec) {
        s->ei = p->exec;
        __atomic_add_fetch(&s->ei->refcount, 1, __ATOMIC_RELAXED);
    }
}

static void snap_release(struct psnap *s) {
    if (s->mm)
        mm_put(s->mm);
    if (s->fds)
        fdtable_put(s->fds);
    path_put(&s->cwd);
    path_put(&s->root);
    mntns_put(s->mntns);
    if (s->ei)
        exec_info_put(s->ei);
}

/* Node ids: id[0] = pid, id[1] = tid (0 at process level), id[2] = fd. */
static int snap(struct snode *n, int want, struct psnap *s) {
    memset(s, 0, sizeof *s);
    s->want = want;
    s->want_tid = n->id[1];
    if (proc_inspect(n->id[0], snap_fn, s) < 0)
        return -ESRCH;
    if (s->want_tid && !s->thread_found) {
        snap_release(s);
        return -ESRCH;
    }
    return 0;
}

static int parse_int(const char *s, int *out) {
    int v = 0;
    if (!*s || (s[0] == '0' && s[1]))
        return -ENOENT;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > 100000000)
            return -ENOENT;
        v = v * 10 + (*s - '0');
    }
    *out = v;
    return 0;
}

/* ---- memory layout ---- */

struct vma_copy {
    uint64_t start, end, pgoff;
    uint32_t prot, flags;
    struct vma_name *name;
};

/* Copies the VMA list (names referenced) so it can be formatted without
 * holding the mm lock. */
static int copy_vmas(struct mm *mm, struct vma_copy **out) {
    int cap = 64;
    for (;;) {
        struct vma_copy *a = kmalloc(sizeof *a * (size_t)cap);
        if (!a)
            return -ENOMEM;
        uint64_t f = spin_lock_irqsave(&mm->lock);
        int n = 0;
        struct vma *v = mm->vmas;
        for (; v && n < cap; v = v->next, n++) {
            a[n] = (struct vma_copy){v->start, v->end, v->pgoff, v->prot, v->flags, v->name};
            if (v->name)
                __atomic_add_fetch(&v->name->refcount, 1, __ATOMIC_RELAXED);
        }
        bool more = v != NULL;
        spin_unlock_irqrestore(&mm->lock, f);
        if (!more) {
            *out = a;
            return n;
        }
        for (int i = 0; i < n; i++) /* too small: again with room for all */
            if (a[i].name)
                vma_name_put(a[i].name);
        kfree(a);
        cap *= 4;
    }
}

static void free_vmas(struct vma_copy *a, int n) {
    for (int i = 0; i < n; i++)
        if (a[i].name)
            vma_name_put(a[i].name);
    kfree(a);
}

struct mem_stats {
    uint64_t vsize, rss, stack, exe, lib, data, start_code, end_code, start_stack;
};

static void mem_stats(struct mm *mm, struct exec_info *ei, struct mem_stats *m) {
    memset(m, 0, sizeof *m);
    if (!mm)
        return;
    struct vma_copy *a;
    int n = copy_vmas(mm, &a);
    if (n < 0)
        return;
    for (int i = 0; i < n; i++) {
        uint64_t len = a[i].end - a[i].start;
        m->vsize += len;
        bool is_exe = a[i].name && ei && !strcmp(a[i].name->path, ei->exe);
        if (a[i].flags & VMA_GROWSDOWN) {
            m->stack += len;
            m->start_stack = a[i].end;
        } else if (is_exe && (a[i].prot & PROT_EXEC)) {
            m->exe += len;
            if (!m->start_code)
                m->start_code = a[i].start;
            m->end_code = a[i].end;
        } else if (a[i].name && (a[i].prot & PROT_EXEC)) {
            m->lib += len;
        } else if (!(a[i].flags & (VMA_SHARED | VMA_PHYS)) && (a[i].prot & PROT_WRITE)) {
            m->data += len;
        }
    }
    free_vmas(a, n);
    m->rss = mm_rss_pages(mm);
}

static int show_maps(struct snode *n, struct sbuf *b) {
    struct psnap s;
    int r = snap(n, W_MM, &s);
    if (r < 0)
        return r;
    if (s.mm) {
        struct vma_copy *a;
        int cnt = copy_vmas(s.mm, &a);
        uint64_t brk_start = s.mm->brk_start, brk = ALIGN_UP(s.mm->brk, PAGE_SIZE);
        for (int i = 0; i < cnt; i++) {
            struct vma_copy *v = &a[i];
            /* Linux merges adjacent anonymous mappings with the same
             * attributes (each brk step is a VMA of its own here). */
#define IN_HEAP(x) ((x)->start >= brk_start && (x)->end <= brk && brk > brk_start)
            while (i + 1 < cnt && !v->name && !a[i + 1].name && a[i + 1].start == v->end &&
                   a[i + 1].prot == v->prot && a[i + 1].flags == v->flags && !(v->flags & VMA_SHARED) &&
                   IN_HEAP(v) == IN_HEAP(&a[i + 1])) {
                a[i + 1].start = v->start;
                v = &a[++i];
            }
            /* Linux's exact layout: the name starts at column 73. */
            sb_printf(b, "%08lx-%08lx %c%c%c%c %08lx %02x:%02x %lu ", v->start, v->end,
                      v->prot & PROT_READ ? 'r' : '-', v->prot & PROT_WRITE ? 'w' : '-',
                      v->prot & PROT_EXEC ? 'x' : '-', v->flags & VMA_SHARED ? 's' : 'p',
                      v->name ? v->pgoff * PAGE_SIZE : 0, 0, v->name ? 1 : 0, v->name ? v->name->ino : 0);
            const char *label = v->name                    ? v->name->path
                                : (v->flags & VMA_GROWSDOWN) ? "[stack]"
                                : IN_HEAP(v)                 ? "[heap]"
                                                             : NULL;
#undef IN_HEAP
            if (label) {
                sb_pad(b, 72);
                sb_printf(b, " %s", label);
            }
            sb_put(b, "\n", 1);
        }
        if (cnt >= 0)
            free_vmas(a, cnt);
    }
    snap_release(&s);
    return 0;
}

/* ---- per-process files ---- */

static uint32_t tty_nr(int ctty) {
    if (ctty == 0)
        return (5 << 8) | 1; /* /dev/console */
    if (ctty > 0) {
        uint32_t minor = (uint32_t)(ctty - 1);
        return (136 << 8) | (minor & 0xff) | ((minor & ~0xffu) << 12);
    }
    return 0;
}

static int show_stat(struct snode *n, struct sbuf *b) {
    struct psnap s;
    int r = snap(n, W_MM | W_EXEC, &s);
    if (r < 0)
        return r;
    struct mem_stats m;
    mem_stats(s.mm, s.ei, &m);
    sb_printf(b, "%d (%s) %c %d %d %d %u %d %u %u %u %u %u %lu %lu %d %d %d %d %d %d %lu %lu %lu %lu", s.tid,
              s.comm, s.state, s.ppid, s.pgid, s.sid, tty_nr(s.ctty), -1, 0x400000u /* PF_RANDOMIZE */, 0u, 0u,
              0u, 0u, ticks_to_clk(s.utime), ticks_to_clk(s.stime), 0, 0, 20, 0, s.nthreads, 0,
              (uint64_t)(s.start_ns / (1000000000ULL / USER_HZ)), m.vsize, m.rss, (uint64_t)-1);
    sb_printf(b, " %lu %lu %lu 0 0 %lu %lu %lu %lu 0 0 0 17 %d 0 0 0 0 0 0 0 0 0 0 0 0 0\n", m.start_code,
              m.end_code, m.start_stack, s.sigpnd | s.shdpnd, s.sigblk, s.sigign, s.sigcgt, s.cpu);
    snap_release(&s);
    return 0;
}

static int show_statm(struct snode *n, struct sbuf *b) {
    struct psnap s;
    int r = snap(n, W_MM | W_EXEC, &s);
    if (r < 0)
        return r;
    struct mem_stats m;
    mem_stats(s.mm, s.ei, &m);
    sb_printf(b, "%lu %lu 0 %lu 0 %lu 0\n", m.vsize / PAGE_SIZE, m.rss, m.exe / PAGE_SIZE,
              (m.data + m.stack) / PAGE_SIZE);
    snap_release(&s);
    return 0;
}

static const char *state_name(char c) {
    switch (c) {
    case 'R': return "R (running)";
    case 'Z': return "Z (zombie)";
    case 'X': return "X (dead)";
    default: return "S (sleeping)";
    }
}

static void cpu_mask(struct sbuf *b) {
    /* comma-separated 32-bit groups, most significant first */
    int groups = (int)((cpu_count + 31) / 32);
    for (int g = groups - 1; g >= 0; g--) {
        uint32_t w = 0;
        for (uint32_t i = 0; i < 32; i++)
            if ((uint32_t)g * 32 + i < cpu_count)
                w |= 1u << i;
        if (groups > 1)
            sb_printf(b, "%08x%s", w, g ? "," : "");
        else
            sb_printf(b, "%x", w);
    }
}

static int show_status(struct snode *n, struct sbuf *b) {
    struct psnap s;
    int r = snap(n, W_MM | W_EXEC | W_FDS, &s);
    if (r < 0)
        return r;
    struct mem_stats m;
    mem_stats(s.mm, s.ei, &m);
    uint64_t kb = PAGE_SIZE / 1024;
    char umask[8];
    octal(umask, s.umask, 4);
    sb_printf(b, "Name:\t%s\nUmask:\t%s\nState:\t%s\nTgid:\t%d\nNgid:\t0\nPid:\t%d\nPPid:\t%d\nTracerPid:\t0\n",
              s.comm, umask, state_name(s.state), s.pid, s.tid, s.ppid);
    sb_printf(b, "Uid:\t%u\t%u\t%u\t%u\nGid:\t%u\t%u\t%u\t%u\nFDSize:\t%d\nGroups:\t\n", s.uid, s.euid, s.euid,
              s.euid, s.gid, s.egid, s.egid, s.egid, MAX_FDS);
    int chain[PIDNS_MAX];
    int nc = pid_ns_chain(s.gpid, proc_current()->ns.pid, chain, PIDNS_MAX);
    sb_printf(b, "NStgid:");
    for (int i = 0; i < nc; i++)
        sb_printf(b, "\t%d", chain[i]);
    nc = pid_ns_chain(s.gtid, proc_current()->ns.pid, chain, PIDNS_MAX);
    sb_printf(b, "\nNSpid:");
    for (int i = 0; i < nc; i++)
        sb_printf(b, "\t%d", chain[i]);
    sb_printf(b, "\nNSpgid:\t%d\nNSsid:\t%d\n", s.pgid, s.sid);
    if (s.mm) {
        sb_printf(b, "VmPeak:\t%8lu kB\nVmSize:\t%8lu kB\nVmLck:\t%8u kB\nVmPin:\t%8u kB\n", m.vsize / 1024,
                  m.vsize / 1024, 0, 0);
        sb_printf(b, "VmHWM:\t%8lu kB\nVmRSS:\t%8lu kB\nRssAnon:\t%8lu kB\nRssFile:\t%8u kB\nRssShmem:\t%8u kB\n",
                  m.rss * kb, m.rss * kb, m.rss * kb, 0, 0);
        sb_printf(b, "VmData:\t%8lu kB\nVmStk:\t%8lu kB\nVmExe:\t%8lu kB\nVmLib:\t%8lu kB\nVmPTE:\t%8u kB\n",
                  m.data / 1024, m.stack / 1024, m.exe / 1024, m.lib / 1024, 0);
        sb_printf(b, "VmSwap:\t%8u kB\n", 0);
    }
    sb_printf(b, "Threads:\t%d\nSigQ:\t0/%d\n", s.nthreads, 31615);
    sb_printf(b, "SigPnd:\t%016lx\nShdPnd:\t%016lx\nSigBlk:\t%016lx\nSigIgn:\t%016lx\nSigCgt:\t%016lx\n", s.sigpnd,
              s.shdpnd, s.sigblk, s.sigign, s.sigcgt);
    sb_printf(b, "CapInh:\t%016x\nCapPrm:\t%016lx\nCapEff:\t%016lx\nCapBnd:\t%016lx\nCapAmb:\t%016x\n", 0,
              0x1ffffffffffUL, 0x1ffffffffffUL, 0x1ffffffffffUL, 0);
    sb_printf(b, "NoNewPrivs:\t0\nSeccomp:\t0\nSeccomp_filters:\t0\nSpeculation_Store_Bypass:\tnot vulnerable\n");
    sb_printf(b, "Cpus_allowed:\t");
    cpu_mask(b);
    sb_printf(b, "\nCpus_allowed_list:\t0");
    if (cpu_count > 1)
        sb_printf(b, "-%u", cpu_count - 1);
    sb_printf(b, "\nMems_allowed:\t1\nMems_allowed_list:\t0\n");
    sb_printf(b, "voluntary_ctxt_switches:\t0\nnonvoluntary_ctxt_switches:\t0\n");
    snap_release(&s);
    return 0;
}

static int show_strings(struct snode *n, struct sbuf *b, bool env) {
    struct psnap s;
    int r = snap(n, W_EXEC, &s);
    if (r < 0)
        return r;
    if (s.ei) /* zombies and kernel-made processes read as empty, as on Linux */
        sb_put(b, env ? s.ei->environ : s.ei->cmdline, env ? s.ei->environ_len : s.ei->cmdline_len);
    snap_release(&s);
    return 0;
}
static int show_cmdline(struct snode *n, struct sbuf *b) { return show_strings(n, b, false); }
static int show_environ(struct snode *n, struct sbuf *b) { return show_strings(n, b, true); }

static int show_comm(struct snode *n, struct sbuf *b) {
    struct psnap s;
    int r = snap(n, 0, &s);
    if (r < 0)
        return r;
    sb_printf(b, "%s\n", s.comm);
    return 0;
}

struct comm_set {
    int tid;
    char name[16];
    bool done;
};

static void comm_set_fn(struct process *p, void *arg) {
    struct comm_set *c = arg;
    int tid = c->tid ? c->tid : p->pid;
    for (struct user_thread *ut = p->threads; ut; ut = ut->next_in_proc) {
        if (ut->tid != tid)
            continue;
        ksnprintf(ut->thread->name, sizeof ut->thread->name, "%s", c->name);
        if (tid == p->pid)
            memcpy(p->name, c->name, sizeof p->name);
        c->done = true;
    }
}

/* Chromium and others name their threads by writing here (like PR_SET_NAME). */
static int64_t store_comm(struct snode *n, const char *buf, size_t len) {
    struct comm_set c = {.tid = n->id[1]};
    size_t l = len < 15 ? len : 15;
    memcpy(c.name, buf, l);
    while (l && (c.name[l - 1] == '\n'))
        l--;
    c.name[l] = 0;
    if (proc_inspect(n->id[0], comm_set_fn, &c) < 0 || !c.done)
        return -ESRCH;
    return (int64_t)len;
}

static int link_exe(struct snode *n, char *buf, size_t size) {
    struct psnap s;
    int r = snap(n, W_EXEC, &s);
    if (r < 0)
        return r;
    r = s.ei ? ksnprintf(buf, size, "%s", s.ei->exe) : -ENOENT;
    snap_release(&s);
    return r;
}

static int follow_exe(struct snode *n, struct path *out) {
    struct psnap s;
    int r = snap(n, W_EXEC, &s);
    if (r < 0)
        return r;
    r = s.ei ? vfs_lookup_path(NULL, s.ei->exe, LOOKUP_FOLLOW, out) : -ENOENT;
    snap_release(&s);
    return r;
}

static int link_cwd(struct snode *n, char *buf, size_t size) {
    struct psnap s;
    int r = snap(n, W_CWD, &s);
    if (r < 0)
        return r;
    r = s.cwd.v ? path_name_here(&s.cwd, buf, size) : -ENOENT;
    snap_release(&s);
    return r;
}

static int follow_cwd(struct snode *n, struct path *out) {
    struct psnap s;
    int r = snap(n, W_CWD, &s);
    if (r < 0)
        return r;
    if (!s.cwd.v) {
        snap_release(&s);
        return -ENOENT;
    }
    *out = s.cwd;
    path_get(out);
    snap_release(&s);
    return 0;
}

static int link_root(struct snode *n, char *buf, size_t size) {
    struct psnap s;
    int r = snap(n, W_CWD, &s);
    if (r < 0)
        return r;
    r = s.root.v ? path_name_here(&s.root, buf, size) : ksnprintf(buf, size, "/");
    snap_release(&s);
    return r;
}

static int follow_root(struct snode *n, struct path *out) {
    struct psnap s;
    int r = snap(n, W_CWD, &s);
    if (r < 0)
        return r;
    if (!s.root.v) {
        snap_release(&s);
        return -ENOENT;
    }
    *out = s.root; /* the process's own root, in its own mount namespace */
    path_get(out);
    snap_release(&s);
    return 0;
}

/* ---- descriptors ---- */

/* The open file at descriptor id[2] (referenced), or NULL. */
static struct file *fd_file(struct snode *n, int fd) {
    struct psnap s;
    if (snap(n, W_FDS, &s) < 0)
        return NULL;
    struct file *f = NULL;
    if (s.fds && fd >= 0 && fd < MAX_FDS) {
        uint64_t fl = spin_lock_irqsave(&s.fds->lock);
        f = s.fds->fd[fd];
        if (f)
            file_ref(f);
        spin_unlock_irqrestore(&s.fds->lock, fl);
    }
    snap_release(&s);
    return f;
}

static int fd_nth(struct snode *dir, uint64_t i, char *name, size_t size, int *id) {
    struct psnap s;
    if (snap(dir, W_FDS, &s) < 0)
        return 0;
    int found = -1;
    if (s.fds) {
        uint64_t fl = spin_lock_irqsave(&s.fds->lock);
        for (int fd = 0; fd < MAX_FDS; fd++)
            if (s.fds->fd[fd] && i-- == 0) {
                found = fd;
                break;
            }
        spin_unlock_irqrestore(&s.fds->lock, fl);
    }
    snap_release(&s);
    if (found < 0)
        return 0;
    ksnprintf(name, size, "%d", found);
    *id = found;
    return 1;
}

static int fd_find(struct snode *dir, const char *name, int *id) {
    int fd;
    if (parse_int(name, &fd) < 0)
        return -ENOENT;
    struct file *f = fd_file(dir, fd);
    if (!f)
        return -ENOENT;
    file_put(f);
    *id = fd;
    return 0;
}

static int link_fd(struct snode *n, char *buf, size_t size) {
    struct file *f = fd_file(n, n->id[2]);
    if (!f)
        return -ENOENT;
    int r = file_describe(f, buf, size);
    file_put(f);
    return r;
}

static int follow_fd(struct snode *n, struct path *out) {
    struct file *f = fd_file(n, n->id[2]);
    if (!f)
        return -ENOENT;
    out->mnt = mount_get(f->mnt);
    out->v = f->vnode;
    vnode_ref(out->v);
    file_put(f);
    return 0;
}

static int show_fdinfo(struct snode *n, struct sbuf *b) {
    struct file *f = fd_file(n, n->id[2]);
    if (!f)
        return -ENOENT;
    char flags[16];
    octal(flags, f->flags, 7);
    sb_printf(b, "pos:\t%lu\nflags:\t%s\nmnt_id:\t%d\nino:\t%lu\n", f->offset, flags, mount_id(f->mnt), f->vnode->ino);
    file_put(f);
    return 0;
}

/* ---- threads ---- */

struct tid_list {
    int *tids;
    int max, n;
};

static void tids_fn(struct process *p, void *arg) {
    struct tid_list *l = arg;
    for (struct user_thread *ut = p->threads; ut && l->n < l->max; ut = ut->next_in_proc)
        l->tids[l->n++] = ut->tid;
}

static void sort_ints(int *a, int n) {
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && a[j - 1] > a[j]; j--) {
            int t = a[j];
            a[j] = a[j - 1];
            a[j - 1] = t;
        }
}

static int task_nth(struct snode *dir, uint64_t i, char *name, size_t size, int *id) {
    struct tid_list l = {kmalloc(1024 * sizeof(int)), 1024, 0};
    if (!l.tids)
        return 0;
    int r = 0;
    if (proc_inspect(dir->id[0], tids_fn, &l) == 0) {
        int k = 0;
        for (int j = 0; j < l.n; j++) /* the ones visible from here, by their numbers here */
            if (pid_to_user(l.tids[j]) > 0)
                l.tids[k++] = l.tids[j];
        sort_ints(l.tids, k);
        if (i < (uint64_t)k) {
            *id = l.tids[i];
            ksnprintf(name, size, "%d", pid_to_user(*id));
            r = 1;
        }
    }
    kfree(l.tids);
    return r;
}

/* "." and ".." and one per thread, as on Linux: Chromium decides whether
 * it is single-threaded from this number (3). */
static int task_links(struct snode *dir) {
    struct psnap s;
    struct snode t = *dir;
    t.id[1] = 0;
    if (snap(&t, 0, &s) < 0)
        return 2;
    int n = 2 + s.nthreads;
    snap_release(&s);
    return n;
}

static int task_find(struct snode *dir, const char *name, int *id) {
    int tid;
    if (parse_int(name, &tid) < 0 || !(tid = pid_from_user(tid)))
        return -ENOENT;
    struct snode t = *dir;
    t.id[1] = tid;
    struct psnap s;
    if (snap(&t, 0, &s) < 0)
        return -ENOENT;
    *id = tid;
    return 0;
}

/* ---- mounts, limits and other per-process text ---- */

/* The process's mounts, named from its own root. */
static int show_mount_table(struct snode *n, struct sbuf *b, bool info) {
    struct psnap s;
    int r = snap(n, W_CWD, &s);
    if (r < 0)
        return r;
    mounts_show(s.mntns, s.root.v ? &s.root : NULL, b, info);
    snap_release(&s);
    return 0;
}

static int show_mounts(struct snode *n, struct sbuf *b) { return show_mount_table(n, b, false); }
static int show_mountinfo(struct snode *n, struct sbuf *b) { return show_mount_table(n, b, true); }

static void limit_row(struct sbuf *b, const char *name, uint64_t soft, uint64_t hard, const char *units) {
    char s[24], h[24];
    if (soft == UINT64_MAX)
        ksnprintf(s, sizeof s, "unlimited");
    else
        ksnprintf(s, sizeof s, "%lu", soft);
    if (hard == UINT64_MAX)
        ksnprintf(h, sizeof h, "unlimited");
    else
        ksnprintf(h, sizeof h, "%lu", hard);
    sb_printf(b, "%s", name);
    sb_pad(b, 26);
    sb_printf(b, "%s", s);
    sb_pad(b, 47);
    sb_printf(b, "%s", h);
    sb_pad(b, 68);
    sb_printf(b, "%s", units);
    if (*units)
        sb_pad(b, 78);
    sb_put(b, "\n", 1);
}

/* The same values prlimit64 reports (linux/sys_proc.c). */
static int show_limits(struct snode *n, struct sbuf *b) {
    struct psnap s;
    int r = snap(n, 0, &s);
    if (r < 0)
        return r;
    const uint64_t U = UINT64_MAX;
    sb_printf(b, "Limit");
    sb_pad(b, 26);
    sb_printf(b, "Soft Limit");
    sb_pad(b, 47);
    sb_printf(b, "Hard Limit");
    sb_pad(b, 68);
    sb_printf(b, "Units");
    sb_pad(b, 78);
    sb_put(b, "\n", 1);
    limit_row(b, "Max cpu time", U, U, "seconds");
    limit_row(b, "Max file size", U, U, "bytes");
    limit_row(b, "Max data size", U, U, "bytes");
    limit_row(b, "Max stack size", USER_STACK_SIZE, U, "bytes");
    limit_row(b, "Max core file size", U, U, "bytes");
    limit_row(b, "Max resident set", U, U, "bytes");
    limit_row(b, "Max processes", U, U, "processes");
    limit_row(b, "Max open files", MAX_FDS, MAX_FDS, "files");
    limit_row(b, "Max locked memory", U, U, "bytes");
    limit_row(b, "Max address space", U, U, "bytes");
    limit_row(b, "Max file locks", U, U, "locks");
    limit_row(b, "Max pending signals", U, U, "signals");
    limit_row(b, "Max msgqueue size", U, U, "bytes");
    limit_row(b, "Max nice priority", U, U, "");
    limit_row(b, "Max realtime priority", U, U, "");
    limit_row(b, "Max realtime timeout", U, U, "us");
    return 0;
}

static int show_zero(struct snode *n, struct sbuf *b) {
    struct psnap s;
    int r = snap(n, 0, &s);
    if (r == 0)
        sb_printf(b, "0\n");
    return r;
}

/* oom_score_adj and oom_adj: there is no OOM killer to tune; accepted. */
static int64_t store_ignored(struct snode *n, const char *buf, size_t len) {
    (void)n;
    (void)buf;
    return (int64_t)len;
}

static int show_cgroup(struct snode *n, struct sbuf *b) {
    (void)n;
    sb_printf(b, "0::/\n");
    return 0;
}

#define FILE_RO(nm, fn)    {.name = nm, .type = VN_REG, .mode = 0444, .show = fn}
#define FILE_RW(nm, fn, w) {.name = nm, .type = VN_REG, .mode = 0644, .show = fn, .store = w}
#define LINK(nm, fn)       {.name = nm, .type = VN_LNK, .mode = 0777, .link = fn}
#define MAGIC(nm, fn, fo)  {.name = nm, .type = VN_LNK, .mode = 0777, .link = fn, .follow = fo}

static const struct sent fd_ent = MAGIC("fd", link_fd, follow_fd);
static const struct sent fdinfo_ent = FILE_RO("fdinfo", show_fdinfo);

#define FD_DIR                                                                                           \
    {.name = "fd", .type = VN_DIR, .mode = 0500, .dyn_ent = &fd_ent, .dyn_slot = 2, .dyn_nth = fd_nth,   \
     .dyn_find = fd_find}
#define FDINFO_DIR                                                                                       \
    {.name = "fdinfo", .type = VN_DIR, .mode = 0500, .dyn_ent = &fdinfo_ent, .dyn_slot = 2,              \
     .dyn_nth = fd_nth, .dyn_find = fd_find}

/* ---- namespaces: /proc/<pid>/ns/<name>, uid_map, gid_map, setgroups ---- */

struct ns_take {
    int type;
    void *obj;
    uint64_t id;
    struct userns *user;
};

static void ns_take_fn(struct process *p, void *arg) {
    struct ns_take *t = arg;
    if (t->type >= 0)
        t->obj = ns_ref_locked(p, t->type, &t->id);
    else
        t->user = userns_ref_locked(p);
}

static int link_ns(struct snode *n, char *buf, size_t size) {
    struct ns_take t = {.type = ns_type_by_name(n->ent->name)};
    if (t.type < 0 || proc_inspect(n->id[0], ns_take_fn, &t) < 0 || !t.obj)
        return -ENOENT;
    int r = ns_text(t.type, t.id, buf, size);
    ns_unref(t.type, t.obj);
    return r;
}

static int follow_ns(struct snode *n, struct path *out) {
    struct ns_take t = {.type = ns_type_by_name(n->ent->name)};
    if (t.type < 0 || proc_inspect(n->id[0], ns_take_fn, &t) < 0 || !t.obj)
        return -ENOENT;
    out->mnt = NULL;
    return ns_make_vnode(t.type, t.obj, t.id, &out->v);
}

static const struct sent ns_children[] = {
    MAGIC("cgroup", link_ns, follow_ns),
    MAGIC("ipc", link_ns, follow_ns),
    MAGIC("mnt", link_ns, follow_ns),
    MAGIC("net", link_ns, follow_ns),
    MAGIC("pid", link_ns, follow_ns),
    MAGIC("pid_for_children", link_ns, follow_ns),
    MAGIC("time", link_ns, follow_ns),
    MAGIC("time_for_children", link_ns, follow_ns),
    MAGIC("user", link_ns, follow_ns),
    MAGIC("uts", link_ns, follow_ns),
    {0},
};

static int map_kind(struct snode *n) {
    const char *nm = n->ent->name;
    return !strcmp(nm, "uid_map") ? 0 : !strcmp(nm, "gid_map") ? 1 : 2;
}

static int show_idmap(struct snode *n, struct sbuf *b) {
    struct ns_take t = {.type = -1};
    if (proc_inspect(n->id[0], ns_take_fn, &t) < 0)
        return -ESRCH;
    ns_show_map(t.user, map_kind(n), b);
    userns_release(t.user);
    return 0;
}

static int64_t store_idmap(struct snode *n, const char *buf, size_t len) {
    struct ns_take t = {.type = -1};
    if (proc_inspect(n->id[0], ns_take_fn, &t) < 0)
        return -ESRCH;
    int64_t r = ns_store_map(t.user, map_kind(n), buf, len);
    userns_release(t.user);
    return r;
}


/* /proc/<pid>/task/<tid>/ */
static const struct sent tid_children[] = {
    {.name = "ns", .type = VN_DIR, .mode = 0511, .children = ns_children},
    FILE_RW("gid_map", show_idmap, store_idmap),
    FILE_RW("setgroups", show_idmap, store_idmap),
    FILE_RW("uid_map", show_idmap, store_idmap),
    FILE_RO("cgroup", show_cgroup),
    FILE_RO("cmdline", show_cmdline),
    FILE_RW("comm", show_comm, store_comm),
    MAGIC("cwd", link_cwd, follow_cwd),
    FILE_RO("environ", show_environ),
    MAGIC("exe", link_exe, follow_exe),
    FD_DIR,
    FDINFO_DIR,
    FILE_RO("limits", show_limits),
    FILE_RO("maps", show_maps),
    FILE_RO("mountinfo", show_mountinfo),
    FILE_RO("mounts", show_mounts),
    MAGIC("root", link_root, follow_root),
    FILE_RO("stat", show_stat),
    FILE_RO("statm", show_statm),
    FILE_RO("status", show_status),
    {0},
};
static const struct sent tid_ent = {.name = "task", .type = VN_DIR, .mode = 0555, .children = tid_children};

/* /proc/<pid>/ */
static const struct sent pid_children[] = {
    {.name = "ns", .type = VN_DIR, .mode = 0511, .children = ns_children},
    FILE_RW("gid_map", show_idmap, store_idmap),
    FILE_RW("setgroups", show_idmap, store_idmap),
    FILE_RW("uid_map", show_idmap, store_idmap),
    FILE_RO("cgroup", show_cgroup),
    FILE_RO("cmdline", show_cmdline),
    FILE_RW("comm", show_comm, store_comm),
    MAGIC("cwd", link_cwd, follow_cwd),
    FILE_RO("environ", show_environ),
    MAGIC("exe", link_exe, follow_exe),
    FD_DIR,
    FDINFO_DIR,
    FILE_RO("limits", show_limits),
    FILE_RO("maps", show_maps),
    FILE_RO("mountinfo", show_mountinfo),
    FILE_RO("mounts", show_mounts),
    FILE_RW("oom_adj", show_zero, store_ignored),
    FILE_RO("oom_score", show_zero),
    FILE_RW("oom_score_adj", show_zero, store_ignored),
    MAGIC("root", link_root, follow_root),
    FILE_RO("stat", show_stat),
    FILE_RO("statm", show_statm),
    FILE_RO("status", show_status),
    {.name = "task", .type = VN_DIR, .mode = 0555, .dyn_ent = &tid_ent, .dyn_slot = 1, .dyn_nth = task_nth,
     .dyn_find = task_find, .links = task_links},
    FILE_RO("wchan", show_zero),
    {0},
};
static const struct sent pid_ent = {.name = "pid", .type = VN_DIR, .mode = 0555, .children = pid_children};

static int pid_nth(struct snode *dir, uint64_t i, char *name, size_t size, int *id) {
    (void)dir;
    int *pids = kmalloc(4096 * sizeof(int));
    if (!pids)
        return 0;
    int n = proc_list_pids(pids, 4096), r = 0, m = 0;
    for (int j = 0; j < n; j++) /* the kernel's own process, and those outside our pid namespace, have none */
        if (pids[j] > 0 && pid_to_user(pids[j]) > 0)
            pids[m++] = pids[j];
    sort_ints(pids, m);
    if (i < (uint64_t)m) {
        *id = pids[i];
        ksnprintf(name, size, "%d", pid_to_user(*id));
        r = 1;
    }
    kfree(pids);
    return r;
}

static void exists_fn(struct process *p, void *arg) {
    (void)p;
    (void)arg;
}

static int pid_find(struct snode *dir, const char *name, int *id) {
    (void)dir;
    int pid;
    if (parse_int(name, &pid) < 0 || pid <= 0 || !(pid = pid_from_user(pid)) || proc_inspect(pid, exists_fn, NULL) < 0)
        return -ENOENT;
    *id = pid;
    return 0;
}

static int link_self(struct snode *n, char *buf, size_t size) {
    (void)n;
    return ksnprintf(buf, size, "%d", pid_to_user(proc_current()->pid));
}

static int link_thread_self(struct snode *n, char *buf, size_t size) {
    (void)n;
    struct thread *t = thread_current();
    int tid = t->user ? t->user->tid : 0;
    return ksnprintf(buf, size, "%d/task/%d", pid_to_user(proc_current()->pid), pid_to_user(tid));
}

static int link_mounts(struct snode *n, char *buf, size_t size) {
    (void)n;
    return ksnprintf(buf, size, "self/mounts");
}

/* ---- system-wide files ---- */

static int show_cpuinfo(struct snode *n, struct sbuf *b) {
    (void)n;
    arch_cpuinfo(b);
    return 0;
}

static void meminfo_row(struct sbuf *b, const char *name, uint64_t kb) {
    sb_printf(b, "%s:", name);
    sb_pad(b, 15);
    sb_printf(b, " %8lu kB\n", kb);
}

static int show_meminfo(struct snode *n, struct sbuf *b) {
    (void)n;
    uint64_t total = pmm_total_pages() * (PAGE_SIZE / 1024), free = pmm_free_pages() * (PAGE_SIZE / 1024);
    meminfo_row(b, "MemTotal", total);
    meminfo_row(b, "MemFree", free);
    meminfo_row(b, "MemAvailable", free);
    static const char *const zero[] = {
        "Buffers", "Cached", "SwapCached", "Active", "Inactive", "Active(anon)", "Inactive(anon)", "Active(file)",
        "Inactive(file)", "Unevictable", "Mlocked", "SwapTotal", "SwapFree", "Dirty", "Writeback", "AnonPages",
        "Mapped", "Shmem", "KReclaimable", "Slab", "SReclaimable", "SUnreclaim", "KernelStack", "PageTables",
        "NFS_Unstable", "Bounce", "WritebackTmp",
    };
    for (size_t i = 0; i < ARRAY_LEN(zero); i++)
        meminfo_row(b, zero[i], 0);
    meminfo_row(b, "CommitLimit", total);
    meminfo_row(b, "Committed_AS", total - free);
    meminfo_row(b, "VmallocTotal", 34359738367);
    meminfo_row(b, "VmallocUsed", 0);
    meminfo_row(b, "VmallocChunk", 0);
    meminfo_row(b, "AnonHugePages", 0);
    sb_printf(b, "HugePages_Total:       0\nHugePages_Free:        0\nHugePages_Rsvd:        0\n"
                 "HugePages_Surp:        0\n");
    meminfo_row(b, "Hugepagesize", 2048);
    meminfo_row(b, "Hugetlb", 0);
    return 0;
}

static int show_stat_sys(struct snode *n, struct sbuf *b) {
    (void)n;
    uint64_t t[CT_N] = {0};
    for (uint32_t i = 0; i < cpu_count; i++)
        for (int k = 0; k < CT_N; k++)
            t[k] += cpu_time[i][k];
    sb_printf(b, "cpu  %lu 0 %lu %lu 0 0 0 0 0 0\n", ticks_to_clk(t[CT_USER]), ticks_to_clk(t[CT_SYS]),
              ticks_to_clk(t[CT_IDLE]));
    for (uint32_t i = 0; i < cpu_count; i++)
        sb_printf(b, "cpu%u %lu 0 %lu %lu 0 0 0 0 0 0\n", i, ticks_to_clk(cpu_time[i][CT_USER]),
                  ticks_to_clk(cpu_time[i][CT_SYS]), ticks_to_clk(cpu_time[i][CT_IDLE]));
    uint64_t up = time_monotonic_ns() / 1000000000ULL;
    sb_printf(b, "intr 0\nctxt 0\nbtime %ld\nprocesses %d\nprocs_running 1\nprocs_blocked 0\nsoftirq 0 0 0 0 0 0 0 0 0 0 0\n",
              time_realtime_s() - (int64_t)up, proc_last_pid());
    return 0;
}

static int show_uptime(struct snode *n, struct sbuf *b) {
    (void)n;
    uint64_t ms = time_monotonic_ns() / 1000000ULL, idle = 0;
    for (uint32_t i = 0; i < cpu_count; i++)
        idle += cpu_time[i][CT_IDLE];
    uint64_t idle_cs = idle * 100 / HZ;
    sb_printf(b, "%lu.%02lu %lu.%02lu\n", ms / 1000, ms % 1000 / 10, idle_cs / 100, idle_cs % 100);
    return 0;
}

static int show_loadavg(struct snode *n, struct sbuf *b) {
    (void)n;
    for (int i = 0; i < 3; i++) {
        uint64_t v = loadavg_fixed[i] + 2048 / 200; /* round to hundredths */
        sb_printf(b, "%lu.%02lu ", v >> 11, ((v & 2047) * 100) >> 11);
    }
    int *all = kmalloc(4096 * sizeof(int));
    int count = all ? proc_list_pids(all, 4096) : 0;
    kfree(all);
    sb_printf(b, "1/%d %d\n", count, proc_last_pid());
    return 0;
}

static int show_version(struct snode *n, struct sbuf *b) {
    (void)n;
    sb_printf(b, "Linux version 6.1.0-os (os@os) (gcc) #1 SMP\n");
    return 0;
}

static int show_filesystems(struct snode *n, struct sbuf *b) {
    (void)n;
    sb_printf(b, "nodev\trootfs\nnodev\tramfs\nnodev\tproc\nnodev\tsysfs\n");
    return 0;
}

static int show_partitions(struct snode *n, struct sbuf *b) {
    (void)n;
    sb_printf(b, "major minor  #blocks  name\n\n");
    for (int i = 0; i < 26; i++) {
        char name[8];
        ksnprintf(name, sizeof name, "sd%c", 'a' + i);
        struct blkdev *d = blkdev_find(name);
        if (!d)
            break;
        sb_printf(b, "%4u %7u %10lu %s\n", 8u, (uint32_t)i * 16, d->sectors * d->sector_size / 1024, name);
    }
    return 0;
}

/* /proc/sys */
static char boot_id[37];

static void make_uuid(char *out) {
    uint8_t r[16];
    random_bytes(r, sizeof r);
    r[6] = (r[6] & 0x0f) | 0x40; /* version 4 */
    r[8] = (r[8] & 0x3f) | 0x80; /* RFC 4122 variant */
    ksnprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", r[0], r[1], r[2],
              r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
}

static int show_boot_id(struct snode *n, struct sbuf *b) {
    (void)n;
    sb_printf(b, "%s\n", boot_id);
    return 0;
}

static int show_uuid(struct snode *n, struct sbuf *b) {
    (void)n;
    char u[37];
    make_uuid(u);
    sb_printf(b, "%s\n", u);
    return 0;
}

static int show_hostname(struct snode *n, struct sbuf *b) {
    (void)n;
    sb_printf(b, "%s\n", uts_hostname());
    return 0;
}

static int64_t store_hostname(struct snode *n, const char *buf, size_t len) {
    (void)n;
    size_t l = len < 64 ? len : 64;
    while (l && (buf[l - 1] == '\n' || buf[l - 1] == 0))
        l--;
    char tmp[65] = {0};
    memcpy(tmp, buf, l);
    memcpy(uts_hostname(), tmp, 65);
    return (int64_t)len;
}

/* Constant /proc/sys values: the text is the entry's only content. */
#define CONST(nm, text) {.name = nm, .type = VN_REG, .mode = 0444, .show = show_const_##text}
#define DEF_CONST(id, text)                                                                              \
    static int show_const_##id(struct snode *n, struct sbuf *b) {                                        \
        (void)n;                                                                                         \
        sb_printf(b, "%s\n", text);                                                                      \
        return 0;                                                                                        \
    }
DEF_CONST(linux, "Linux")
DEF_CONST(release, "6.1.0-os")
DEF_CONST(kversion, "#1 SMP")
DEF_CONST(none, "(none)")
DEF_CONST(pid_max, "4194304")
DEF_CONST(threads_max, "65536")
DEF_CONST(cap_last_cap, "40")
DEF_CONST(ngroups_max, "65536")
DEF_CONST(shmmax, "18446744073692774399")
DEF_CONST(shmall, "18446744073692774399")
DEF_CONST(shmmni, "4096")
DEF_CONST(entropy, "256")
DEF_CONST(zero, "0")
DEF_CONST(two, "2")
DEF_CONST(core_pattern, "core")
DEF_CONST(max_map_count, "65530")
DEF_CONST(swappiness, "60")
DEF_CONST(ratio50, "50")
DEF_CONST(mmap_min_addr, "65536")
DEF_CONST(file_max, "9223372036854775807")
DEF_CONST(nr_open, "1048576")
DEF_CONST(file_nr, "0\t0\t9223372036854775807")
DEF_CONST(pipe_max, "1048576")
DEF_CONST(watches, "65536")
DEF_CONST(instances, "128")

DEF_CONST(ns_one, "1")
DEF_CONST(overflow_id, "65534")
DEF_CONST(cap_zero, "0")

static const struct sent sys_kernel_random[] = {
    {.name = "boot_id", .type = VN_REG, .mode = 0444, .show = show_boot_id},
    CONST("entropy_avail", entropy),
    CONST("poolsize", entropy),
    {.name = "uuid", .type = VN_REG, .mode = 0444, .show = show_uuid},
    {0},
};
static const struct sent sys_kernel[] = {
    CONST("cap_last_cap", cap_last_cap),
    CONST("core_pattern", core_pattern),
    CONST("dmesg_restrict", zero),
    CONST("domainname", none),
    {.name = "hostname", .type = VN_REG, .mode = 0644, .show = show_hostname, .store = store_hostname},
    CONST("kptr_restrict", zero),
    CONST("ngroups_max", ngroups_max),
    CONST("overflowgid", overflow_id),
    CONST("overflowuid", overflow_id),
    CONST("osrelease", release),
    CONST("ostype", linux),
    CONST("perf_event_paranoid", two),
    CONST("pid_max", pid_max),
    {.name = "random", .type = VN_DIR, .mode = 0555, .children = sys_kernel_random},
    CONST("shmall", shmall),
    CONST("shmmax", shmmax),
    CONST("shmmni", shmmni),
    CONST("threads-max", threads_max),
    CONST("unprivileged_userns_clone", ns_one),
    CONST("unprivileged_bpf_disabled", ns_one),
    CONST("yama_ptrace_scope", cap_zero),
    CONST("version", kversion),
    {0},
};
DEF_CONST(ns_max, "63785")

static int show_max_userns(struct snode *n, struct sbuf *b) {
    (void)n;
    sb_printf(b, "%d\n", userns_max_get());
    return 0;
}

static int64_t store_max_userns(struct snode *n, const char *buf, size_t len) {
    (void)n;
    int v = 0;
    size_t i = 0;
    for (; i < len && buf[i] >= '0' && buf[i] <= '9'; i++)
        v = v * 10 + (buf[i] - '0');
    if (i == 0)
        return -EINVAL;
    int r = userns_max_set(v);
    return r < 0 ? r : (int64_t)len;
}

static const struct sent sys_user[] = {
    CONST("max_cgroup_namespaces", ns_max),
    CONST("max_ipc_namespaces", ns_max),
    CONST("max_mnt_namespaces", ns_max),
    CONST("max_net_namespaces", ns_max),
    CONST("max_pid_namespaces", ns_max),
    CONST("max_time_namespaces", ns_max),
    FILE_RW("max_user_namespaces", show_max_userns, store_max_userns),
    CONST("max_uts_namespaces", ns_max),
    {0},
};
static const struct sent sys_vm[] = {
    CONST("max_map_count", max_map_count),
    CONST("mmap_min_addr", mmap_min_addr),
    CONST("overcommit_memory", zero),
    CONST("overcommit_ratio", ratio50),
    CONST("swappiness", swappiness),
    {0},
};
static const struct sent sys_fs_inotify[] = {
    CONST("max_user_instances", instances),
    CONST("max_user_watches", watches),
    {0},
};
static const struct sent sys_fs[] = {
    CONST("file-max", file_max),
    CONST("file-nr", file_nr),
    {.name = "inotify", .type = VN_DIR, .mode = 0555, .children = sys_fs_inotify},
    CONST("nr_open", nr_open),
    CONST("pipe-max-size", pipe_max),
    {0},
};
static const struct sent sys_dir[] = {
    {.name = "fs", .type = VN_DIR, .mode = 0555, .children = sys_fs},
    {.name = "kernel", .type = VN_DIR, .mode = 0555, .children = sys_kernel},
    {.name = "user", .type = VN_DIR, .mode = 0555, .children = sys_user},
    {.name = "vm", .type = VN_DIR, .mode = 0555, .children = sys_vm},
    {0},
};

static const struct sent net_children[] = {
    FILE_RO("dev", procnet_dev),
    FILE_RO("route", procnet_route),
    {0},
};

static const struct sent root_children[] = {
    FILE_RO("cpuinfo", show_cpuinfo),
    FILE_RO("filesystems", show_filesystems),
    FILE_RO("loadavg", show_loadavg),
    FILE_RO("meminfo", show_meminfo),
    LINK("mounts", link_mounts),
    {.name = "net", .type = VN_DIR, .mode = 0555, .children = net_children},
    FILE_RO("partitions", show_partitions),
    LINK("self", link_self),
    FILE_RO("stat", show_stat_sys),
    {.name = "sys", .type = VN_DIR, .mode = 0555, .children = sys_dir},
    LINK("thread-self", link_thread_self),
    FILE_RO("uptime", show_uptime),
    FILE_RO("version", show_version),
    {0},
};
static const struct sent proc_root = {.name = "proc", .type = VN_DIR, .mode = 0555, .children = root_children,
                                      .dyn_ent = &pid_ent, .dyn_slot = 0, .dyn_nth = pid_nth, .dyn_find = pid_find};

/* Another /proc (mount -t proc): the same tree. */
struct vnode *procfs_new_root(void);
struct vnode *procfs_new_root(void) { return synth_root(&proc_root); }

void procfs_init(void) {
    arch_cpuinfo_init();
    make_uuid(boot_id);
    struct vnode *root = synth_root(&proc_root);
    KASSERT(root);
    KASSERT(vfs_mount("/proc", root, "proc", MNT_NOSUID | MNT_NODEV | MNT_NOEXEC) == 0);
    vnode_put(root);
    /* The usual links udev makes: bash's <(...) and many scripts use them. */
    static const char *const links[][2] = {
        {"/dev/fd", "/proc/self/fd"}, {"/dev/stdin", "/proc/self/fd/0"},
        {"/dev/stdout", "/proc/self/fd/1"}, {"/dev/stderr", "/proc/self/fd/2"},
    };
    struct vnode *dev;
    if (vfs_lookup(NULL, "/dev", LOOKUP_FOLLOW, &dev) == 0) {
        for (size_t i = 0; i < ARRAY_LEN(links); i++)
            if (dev->ops->symlink)
                dev->ops->symlink(dev, links[i][0] + 5, links[i][1]);
        vnode_put(dev);
    }
}
