#include <asm/elf.h>
#include <kernel/arch.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/ns.h>
#include <kernel/proc.h>
#include <kernel/vfs.h>

/* execve: ELF64 executables (static, static-pie, or dynamic with an
 * interpreter such as ld-musl / ld-linux) and #! scripts. */

#define PIE_BASE    0x0000555555554000ULL
#define MAX_ARGS    1024
#define MAX_SHEBANG 4

struct elf64_ehdr {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct elf64_phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

#define PT_LOAD   1
#define PT_INTERP 3
#define PT_PHDR   6
#define PF_X 1
#define PF_W 2
#define PF_R 4

struct loaded {
    uint64_t entry, base, phdr, phnum, end;
    char interp[256];
};

void fdtable_put(struct fdtable *t);
void random_bytes(void *buf, size_t len);

/* Executables are read in pieces, never whole: Chromium's is over 200 MB,
 * and a buffer that size (physically contiguous, from the kernel heap) is
 * there on some boots and not on others. */
static int file_read_at(struct vnode *v, void *buf, size_t n, uint64_t off) {
    size_t done = 0;
    while (done < n) {
        int64_t r = v->ops->read(v, NULL, (uint8_t *)buf + done, n - done, off + done);
        if (r <= 0)
            return r < 0 ? (int)r : -ENOEXEC; /* the file is shorter than its headers say */
        done += (size_t)r;
    }
    return 0;
}

/* Copies [off, off + n) of the file to `addr` in the new address space. */
static int copy_file_out(struct mm *mm, uint64_t addr, struct vnode *v, uint64_t off, uint64_t n) {
    enum { CHUNK = 64 * 1024 };
    uint8_t *buf = kmalloc(n < CHUNK ? (n ? n : 1) : CHUNK);
    if (!buf)
        return -ENOMEM;
    int r = 0;
    while (n && r == 0) {
        size_t k = n < CHUNK ? (size_t)n : CHUNK;
        r = file_read_at(v, buf, k, off);
        if (r == 0)
            r = mm_copy_out(mm, addr, buf, k);
        addr += k, off += k, n -= k;
    }
    kfree(buf);
    return r;
}

static int load_elf(struct mm *mm, struct vnode *file, uint64_t dyn_base, struct loaded *out, struct vma_name *name) {
    uint64_t len = file->size;
    struct elf64_ehdr ehdr;
    const struct elf64_ehdr *eh = &ehdr;
    if (len < sizeof ehdr || file_read_at(file, &ehdr, sizeof ehdr, 0) < 0)
        return -ENOEXEC;
    if (memcmp(eh->ident, "\x7f" "ELF", 4) || eh->ident[4] != 2 || eh->ident[5] != 1 || eh->machine != ELF_MACHINE ||
        (eh->type != 2 && eh->type != 3) || eh->phentsize != sizeof(struct elf64_phdr) || eh->phnum > 512 ||
        eh->phoff + (uint64_t)eh->phnum * sizeof(struct elf64_phdr) > len)
        return -ENOEXEC;
    struct elf64_phdr *ph = kmalloc(sizeof *ph * (eh->phnum ? eh->phnum : 1));
    if (!ph)
        return -ENOMEM;
    int r = file_read_at(file, ph, sizeof *ph * eh->phnum, eh->phoff);
    if (r < 0) {
        kfree(ph);
        return r;
    }

    uint64_t base = eh->type == 3 ? dyn_base : 0;
    memset(out, 0, sizeof *out);
    out->base = base;
    out->entry = base + eh->entry;
    out->phnum = eh->phnum;

    uint64_t first_load_vaddr = UINT64_MAX; /* vaddr of file offset 0 */
    for (int i = 0; i < eh->phnum && r == 0; i++) {
        const struct elf64_phdr *p = &ph[i];
        if (p->type == PT_INTERP) {
            if (p->filesz >= sizeof out->interp || p->offset + p->filesz > len) {
                r = -ENOEXEC;
                break;
            }
            r = file_read_at(file, out->interp, p->filesz, p->offset);
            out->interp[p->filesz] = 0;
        } else if (p->type == PT_PHDR) {
            out->phdr = base + p->vaddr;
        } else if (p->type == PT_LOAD) {
            if (p->offset + p->filesz > len || p->filesz > p->memsz) {
                r = -ENOEXEC;
                break;
            }
            if (first_load_vaddr == UINT64_MAX)
                first_load_vaddr = p->vaddr - p->offset; /* segments are sorted by vaddr */
            uint32_t prot = (p->flags & PF_R ? PROT_READ : 0) | (p->flags & PF_W ? PROT_WRITE : 0) |
                            (p->flags & PF_X ? PROT_EXEC : 0);
            uint64_t start = ALIGN_DOWN(base + p->vaddr, PAGE_SIZE);
            uint64_t end = ALIGN_UP(base + p->vaddr + p->memsz, PAGE_SIZE);
            /* Adjacent segments may share their boundary page: keep the
             * earlier mapping there and widen its permissions. */
            struct vma *v = mm_find_vma(mm, start);
            if (v) {
                mm_protect(mm, start, start + PAGE_SIZE, v->prot | prot);
                start += PAGE_SIZE;
            }
            r = start < end ? mm_map(mm, start, end, prot, 0) : 0;
            if (r < 0)
                break;
            if (name) /* /proc/<pid>/maps shows the file and offset of each segment */
                mm_set_name(mm, ALIGN_DOWN(base + p->vaddr, PAGE_SIZE), end, name, p->offset / PAGE_SIZE);
            r = copy_file_out(mm, base + p->vaddr, file, p->offset, p->filesz);
            if (base + p->vaddr + p->memsz > out->end)
                out->end = base + p->vaddr + p->memsz;
        }
    }
    /* No PT_PHDR (common in static binaries): headers are in the first
     * loaded page when phoff falls inside it. */
    if (r == 0 && !out->phdr && first_load_vaddr != UINT64_MAX)
        out->phdr = base + first_load_vaddr + eh->phoff;
    kfree(ph);
    return r;
}

/* The file to run: an executable regular file (a reference the caller puts). */
static int open_exec(const char *path, struct vnode **out) {
    struct path p;
    int r = vfs_lookup_path(NULL, path, LOOKUP_FOLLOW, &p);
    if (r < 0)
        return r;
    if (p.v->type != VN_REG || !(p.v->mode & 0111) || (mount_flags(p.mnt) & MNT_NOEXEC)) {
        path_put(&p);
        return -EACCES;
    }
    *out = p.v;
    vnode_ref(p.v);
    path_put(&p);
    return 0;
}

/* Builds argc/argv/envp/auxv on the new stack. Returns the initial rsp. */
static int build_stack(struct mm *mm, char *const argv[], char *const envp[], const struct loaded *exe,
                       const struct loaded *interp, const char *execfn, uint64_t *rsp_out) {
    int argc = 0, envc = 0;
    size_t strings = 0;
    while (argv[argc]) {
        strings += strlen(argv[argc]) + 1;
        if (++argc > MAX_ARGS)
            return -E2BIG;
    }
    while (envp[envc]) {
        strings += strlen(envp[envc]) + 1;
        if (++envc > MAX_ARGS)
            return -E2BIG;
    }
    strings += strlen(execfn) + 1 + sizeof AT_PLATFORM_STR + 16;
    if (strings > 128 * 1024)
        return -E2BIG;

    /* Lay everything out in a kernel buffer that mirrors the stack top. */
    size_t auxc = 18;
    size_t ptrs = 1 + (size_t)argc + 1 + (size_t)envc + 1 + auxc * 2;
    size_t total = ALIGN_UP(strings, 16) + ptrs * 8 + 16;
    uint8_t *buf = kzalloc(total);
    if (!buf)
        return -ENOMEM;
    uint64_t top = USER_STACK_TOP - 64; /* small gap, like Linux */
    uint64_t base = ALIGN_DOWN(top - total, 16);
    uint64_t str_at = top - strings; /* user address of the next string */

#define PUT_STR(s)                                                                                       \
    ({                                                                                                   \
        size_t _n = strlen(s) + 1;                                                                       \
        memcpy(buf + (str_at - base), (s), _n);                                                          \
        uint64_t _a = str_at;                                                                            \
        str_at += _n;                                                                                    \
        _a;                                                                                              \
    })

    uint64_t *vec = (uint64_t *)buf;
    size_t k = 0;
    vec[k++] = (uint64_t)argc;
    for (int i = 0; i < argc; i++)
        vec[k++] = PUT_STR(argv[i]);
    vec[k++] = 0;
    for (int i = 0; i < envc; i++)
        vec[k++] = PUT_STR(envp[i]);
    vec[k++] = 0;
    uint64_t fn = PUT_STR(execfn);
    uint64_t platform = PUT_STR(AT_PLATFORM_STR);
    uint64_t rnd = str_at;
    random_bytes(buf + (rnd - base), 16);
    str_at += 16;

    uint64_t aux[][2] = {
        {AT_PHDR, exe->phdr}, {AT_PHENT, sizeof(struct elf64_phdr)}, {AT_PHNUM, exe->phnum},
        {AT_PAGESZ, PAGE_SIZE}, {AT_BASE, interp ? interp->base : 0}, {AT_FLAGS, 0},
        {AT_ENTRY, exe->entry}, {AT_UID, 0}, {AT_EUID, 0}, {AT_GID, 0}, {AT_EGID, 0},
        {AT_HWCAP, arch_hwcap()}, {AT_CLKTCK, 100}, {AT_SECURE, 0}, {AT_RANDOM, rnd},
        {AT_PLATFORM, platform}, {AT_EXECFN, fn}, {AT_NULL, 0},
    };
    _Static_assert(sizeof aux / sizeof aux[0] == 18, "auxc must match");
    for (size_t i = 0; i < auxc; i++) {
        vec[k++] = aux[i][0];
        vec[k++] = aux[i][1];
    }
#undef PUT_STR
    KASSERT(base + k * 8 <= top - strings);

    int r = mm_copy_out(mm, base, buf, top - base);
    kfree(buf);
    if (r < 0)
        return r;
    *rsp_out = base;
    return 0;
}

static int parse_shebang(const char *img, size_t len, char *interp, size_t isz, char *arg, size_t asz) {
    size_t i = 2, n = 0;
    while (i < len && (img[i] == ' ' || img[i] == '\t'))
        i++;
    while (i < len && img[i] != ' ' && img[i] != '\t' && img[i] != '\n' && n + 1 < isz)
        interp[n++] = img[i++];
    interp[n] = 0;
    while (i < len && (img[i] == ' ' || img[i] == '\t'))
        i++;
    n = 0;
    while (i < len && img[i] != '\n' && n + 1 < asz)
        arg[n++] = img[i++];
    while (n && (arg[n - 1] == ' ' || arg[n - 1] == '\t' || arg[n - 1] == '\r'))
        n--;
    arg[n] = 0;
    return interp[0] ? 0 : -ENOEXEC;
}

static int execve_depth(const char *path, char *const argv[], char *const envp[], struct regs *frame, int depth) {
    struct vnode *file;
    int r = open_exec(path, &file);
    if (r < 0)
        return r;
    char head[600]; /* enough for a #! line */
    size_t hl = file->size < sizeof head ? (size_t)file->size : sizeof head;
    if ((r = file_read_at(file, head, hl, 0)) < 0) {
        vnode_put(file);
        return r;
    }

    if (hl >= 2 && head[0] == '#' && head[1] == '!') {
        char interp[256], arg[256];
        r = parse_shebang(head, hl, interp, sizeof interp, arg, sizeof arg);
        vnode_put(file);
        if (r < 0 || depth >= MAX_SHEBANG)
            return r < 0 ? r : -ELOOP;
        int argc = 0;
        while (argv[argc])
            argc++;
        char **nargv = kmalloc(sizeof(char *) * (size_t)(argc + 3));
        if (!nargv)
            return -ENOMEM;
        int k = 0;
        nargv[k++] = interp;
        if (arg[0])
            nargv[k++] = arg;
        nargv[k++] = (char *)path;
        for (int i = 1; i <= argc; i++)
            nargv[k++] = argv[i]; /* includes the terminating NULL */
        r = execve_depth(interp, nargv, envp, frame, depth + 1);
        kfree(nargv);
        return r;
    }

    struct mm *mm = mm_create();
    if (!mm) {
        vnode_put(file);
        return -ENOMEM;
    }
    /* /proc/<pid>/exe, cmdline, environ, and the names in maps. */
    struct exec_info *ei = exec_info_create(NULL, path, argv, envp);
    struct vma_name *exe_name = ei ? vma_name_lookup(NULL, ei->exe) : NULL, *interp_name = NULL;
    struct loaded exe, interp;
    bool has_interp = false;
    r = ei ? load_elf(mm, file, PIE_BASE, &exe, exe_name) : -ENOMEM;
    vnode_put(file);
    if (r == 0 && exe.interp[0]) {
        struct vnode *ifile;
        r = open_exec(exe.interp, &ifile);
        if (r == 0) {
            interp_name = vma_name_lookup(NULL, exe.interp);
            r = load_elf(mm, ifile, USER_INTERP_BASE, &interp, interp_name);
            vnode_put(ifile);
            has_interp = r == 0;
        }
    }
    if (exe_name)
        vma_name_put(exe_name);
    if (interp_name)
        vma_name_put(interp_name);
    if (r == 0)
        r = mm_map(mm, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_TOP, PROT_READ | PROT_WRITE, VMA_GROWSDOWN);
    uint64_t rsp = 0;
    if (r == 0)
        r = build_stack(mm, argv, envp, &exe, has_interp ? &interp : NULL, path, &rsp);
    if (r < 0) {
        mm_put(mm);
        if (ei)
            exec_info_put(ei);
        return r;
    }
    mm->brk_start = mm->brk = ALIGN_UP(exe.end, PAGE_SIZE);

    /* The new image starts with one thread: the others must be gone before
     * the old address space they run in is freed. */
    struct thread *t = thread_current();
    struct process *p = t->user->proc;
    if (p->nthreads > 1 && (r = proc_kill_other_threads()) < 0) {
        mm_put(mm);
        exec_info_put(ei);
        return r;
    }
    t->user->clear_child_tid = 0; /* it pointed into the old image */

    /* Point of no return: switch the process over to the new image. The
     * swap is under the table lock because /proc takes references on
     * another process's mm and exec info. */
    uint64_t lf = proc_table_lock();
    struct mm *old = p->mm;
    struct exec_info *old_ei = p->exec;
    p->mm = mm;
    p->exec = ei;
    proc_table_unlock(lf);
    mm_activate(mm);
    if (old)
        mm_put(old);
    if (old_ei)
        exec_info_put(old_ei);

    uint64_t f = spin_lock_irqsave(&p->fds->lock);
    for (int i = 0; i < MAX_FDS; i++) {
        if (p->fds->fd[i] && p->fds->cloexec[i]) {
            file_put(p->fds->fd[i]);
            p->fds->fd[i] = NULL;
            p->fds->cloexec[i] = 0;
        }
    }
    spin_unlock_irqrestore(&p->fds->lock, f);
    for (int s = 1; s < NSIG; s++) {
        if (p->sigactions[s].handler != SIG_IGN)
            p->sigactions[s] = (struct linux_sigaction){0};
    }
    const char *slash = path;
    for (const char *c = path; *c; c++)
        if (*c == '/')
            slash = c + 1;
    ksnprintf(p->name, sizeof p->name, "%s", slash);
    ksnprintf(t->name, sizeof t->name, "%s", p->name); /* the thread's comm, as on Linux */
    /* capabilities across exec: root (as its user namespace names it) has them
     * all, anyone else none (there are no file capabilities or setuid files) */
    if (uid_to_user(p->euid) == 0)
        p->cap_eff_off = p->cap_perm_off = 0;
    else if (uid_to_user(p->uid) == 0)
        p->cap_perm_off = 0, p->cap_eff_off = ~0ull;
    else
        p->cap_eff_off = p->cap_perm_off = ~0ull;

    t->user->tls = 0;
    arch_load_tls(0);
    fpu_reset(t->user->fpu);
    fpu_restore(t->user->fpu);

    arch_exec_init_regs(frame, has_interp ? interp.entry : exe.entry, rsp);

    if (!p->vfork_done) {
        p->vfork_done = true;
        waitq_wake_all(&p->vfork_wait);
    }
    return 0;
}

int do_execve(const char *path, char *const argv[], char *const envp[], struct regs *frame) {
    return execve_depth(path, argv, envp, frame, 0);
}
