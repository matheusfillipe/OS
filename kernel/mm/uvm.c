#include <kernel/boot.h>
#include <kernel/cpu.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/proc.h>

/* User address spaces: a sorted VMA list plus page tables whose upper half
 * is shared with the kernel. Pages are allocated on first touch (demand
 * zero). fork shares private pages copy-on-write (PG_COW: read-only in
 * both; the first write takes a copy, break_cow); shared mappings
 * (VMA_SHARED) map a pageobj's pages, which fork shares as they are. Walks
 * skip absent page tables whole (pte_find), so huge reserved ranges cost
 * nothing.
 *
 * Page ownership by mapping kind:
 *   private (anonymous, private file copies, ELF segments, stack): the PTE
 *       holds one reference (two or more processes after a fork, until a
 *       write copies it); unmapping drops it (page_unref).
 *   VMA_SHARED: the PTE holds one reference on the object's page; unmapping
 *       drops it (page_unref). The VMA holds a reference on the object.
 *   VMA_PHYS: device memory, never freed. */

static uint64_t pte_flags_for(uint32_t prot) {
    uint64_t f = PG_USER;
    if (prot & PROT_WRITE)
        f |= PG_WRITE;
    if (prot & PROT_EXEC)
        f |= PG_EXEC;
    return f;
}

struct mm *mm_create(void) {
    struct mm *mm = kzalloc(sizeof *mm);
    if (!mm)
        return NULL;
    uint64_t phys = pmm_alloc(1);
    if (!phys) {
        kfree(mm);
        return NULL;
    }
    mm->pgd = phys_to_virt(phys);
    memset(mm->pgd, 0, PAGE_SIZE / 2);
    memcpy(mm->pgd + 256, vmm_kernel_pgd() + 256, PAGE_SIZE / 2);
    mm->refcount = 1;
    mm->mmap_hint = USER_MMAP_BASE;
    return mm;
}

/* active_mm is published before the root is loaded (tlb_shootdown relies on
 * the order). Interrupts stay off so the root and active_mm change on the
 * same CPU. */
void mm_activate(struct mm *mm) {
    uint64_t phys = virt_to_phys_hhdm(mm->pgd);
    uint64_t f = irq_save();
    __atomic_store_n(&this_cpu()->active_mm, mm, __ATOMIC_SEQ_CST);
    if (arch_current_root() != phys)
        arch_load_root(phys);
    irq_restore(f);
}

void mm_activate_kernel(void) {
    uint64_t kphys = virt_to_phys_hhdm(vmm_kernel_pgd());
    uint64_t f = irq_save();
    if (arch_current_root() != kphys)
        arch_load_root(kphys);
    __atomic_store_n(&this_cpu()->active_mm, NULL, __ATOMIC_SEQ_CST);
    irq_restore(f);
}

/* Returns the PTE slot for `va`, creating tables if `create`. */
static uint64_t *pte_slot(struct mm *mm, uint64_t va, bool create) {
    uint64_t *table = mm->pgd;
    for (int level = 4; level > 1; level--) {
        uint64_t idx = (va >> (12 + 9 * (level - 1))) & 0x1ff;
        if (!pte_present(table[idx])) {
            if (!create)
                return NULL;
            uint64_t p = pmm_alloc(1);
            if (!p)
                return NULL;
            memset(phys_to_virt(p), 0, PAGE_SIZE);
            table[idx] = pte_table(p, true);
        }
        table = phys_to_virt(pte_phys(table[idx]));
    }
    return &table[(va >> 12) & 0x1ff];
}

/* The PTE slot for `va` if its page tables exist. Otherwise NULL, with
 * *span the size of the empty region around `va` that holds no pages at all
 * (so walks over huge reserved ranges skip them whole, not page by page). */
static uint64_t *pte_find(struct mm *mm, uint64_t va, uint64_t *span) {
    uint64_t *table = mm->pgd;
    for (int level = 4; level > 1; level--) {
        uint64_t shift = 12 + 9 * (uint64_t)(level - 1);
        uint64_t idx = (va >> shift) & 0x1ff;
        if (!pte_present(table[idx])) {
            *span = 1ULL << shift;
            return NULL;
        }
        table = phys_to_virt(pte_phys(table[idx]));
    }
    *span = PAGE_SIZE;
    return &table[(va >> 12) & 0x1ff];
}

/* The next address a walk looks at after `va`, given pte_find's span. */
static inline uint64_t walk_next(uint64_t va, uint64_t span) { return ALIGN_DOWN(va, span) + span; }

/* A write to a copy-on-write page (shared since fork): it becomes this
 * address space's own, a copy if anyone else still has it. Lock held. */
static bool break_cow(struct mm *mm, struct vma *v, uint64_t va, uint64_t *pte) {
    uint64_t old = pte_phys(*pte);
    if (page_refcount(old) > 1) {
        uint64_t p = page_alloc_shared(); /* counted: it may be shared by a later fork */
        if (!p)
            return false;
        memcpy(phys_to_virt(p), phys_to_virt(old), PAGE_SIZE);
        *pte = pte_make(p, pte_flags_for(v->prot));
        arch_tlb_flush_page(va);
        tlb_shootdown(mm); /* other threads must stop reading the old page */
        page_unref(old);   /* the other side keeps it */
    } else {
        *pte = pte_make(old, pte_flags_for(v->prot)); /* the last one: just ours again */
        arch_tlb_flush_page(va);
    }
    return true;
}

struct vma *mm_find_vma(struct mm *mm, uint64_t addr) {
    for (struct vma *v = mm->vmas; v; v = v->next)
        if (addr >= v->start && addr < v->end)
            return v;
    return NULL;
}

/* A copy of `v` (for splits and fork); it holds its own object reference. */
static struct vma *vma_dup(const struct vma *v) {
    struct vma *n = kmalloc(sizeof *n);
    if (n) {
        *n = *v;
        n->next = NULL;
        if (n->obj)
            pageobj_ref(n->obj);
        if (n->name)
            __atomic_add_fetch(&n->name->refcount, 1, __ATOMIC_RELAXED);
    }
    return n;
}

static void vma_free(struct vma *v) {
    if (v->obj)
        pageobj_put(v->obj);
    if (v->name)
        vma_name_put(v->name);
    kfree(v);
}

struct vma_name *vma_name_create(const char *path, uint64_t ino) {
    size_t n = strlen(path) + 1;
    struct vma_name *vn = kmalloc(sizeof *vn + n);
    if (vn) {
        vn->refcount = 1;
        vn->ino = ino;
        memcpy(vn->path, path, n);
    }
    return vn;
}

void vma_name_put(struct vma_name *n) {
    if (__atomic_sub_fetch(&n->refcount, 1, __ATOMIC_ACQ_REL) == 0)
        kfree(n);
}

void mm_set_name(struct mm *mm, uint64_t start, uint64_t end, struct vma_name *name, uint64_t pgoff) {
    uint64_t f = spin_lock_irqsave(&mm->lock);
    for (struct vma *v = mm->vmas; v; v = v->next) {
        if (v->start < start || v->end > end)
            continue;
        __atomic_add_fetch(&name->refcount, 1, __ATOMIC_RELAXED);
        if (v->name)
            vma_name_put(v->name);
        v->name = name;
        if (!(v->flags & VMA_SHARED)) /* a shared VMA's pgoff indexes its object */
            v->pgoff = pgoff + (v->start - start) / PAGE_SIZE;
    }
    spin_unlock_irqrestore(&mm->lock, f);
}

/* Walks the lower half's page tables rather than the VMAs: large mostly
 * untouched reservations cost nothing this way. */
static uint64_t count_present(uint64_t *table, int level) {
    uint64_t n = 0;
    for (int i = 0; i < (level == 4 ? 256 : 512); i++) {
        if (!pte_present(table[i]))
            continue;
        if (level == 1)
            n++;
        else
            n += count_present(phys_to_virt(pte_phys(table[i])), level - 1);
    }
    return n;
}

uint64_t mm_rss_pages(struct mm *mm) {
    uint64_t f = spin_lock_irqsave(&mm->lock);
    uint64_t n = count_present(mm->pgd, 4);
    spin_unlock_irqrestore(&mm->lock, f);
    return n;
}

/* Moves `v`'s start up to `start`, keeping the object offset in step. */
static void vma_set_start(struct vma *v, uint64_t start) {
    v->pgoff += (start - v->start) / PAGE_SIZE;
    v->start = start;
}

/* Pages taken out of the page tables are only freed once no CPU can still
 * reach them through a stale TLB entry, i.e. after a shootdown. Batching
 * keeps that to one shootdown per FREE_BATCH pages. Entries with bit 0 set
 * are shared pages, which lose a reference instead of being freed. */
#define FREE_BATCH 64
#define BATCH_SHARED 1ULL
struct free_batch {
    int n;
    bool changed; /* PTEs were cleared: a shootdown is due */
    uint64_t pages[FREE_BATCH];
};

static void batch_flush(struct mm *mm, struct free_batch *b) {
    tlb_shootdown(mm);
    for (int i = 0; i < b->n; i++) {
        if (b->pages[i] & BATCH_SHARED)
            page_unref(b->pages[i] & ~BATCH_SHARED);
        else
            pmm_free(b->pages[i], 1);
    }
    b->n = 0;
    b->changed = false;
}

/* Clears the PTEs of `v` in [s, e) and queues their pages. Lock held. */
static void zap_locked(struct mm *mm, struct vma *v, uint64_t s, uint64_t e, struct free_batch *b) {
    uint64_t span;
    for (uint64_t va = s; va < e; va = walk_next(va, span)) {
        uint64_t *pte = pte_find(mm, va, &span);
        if (!pte || !pte_present(*pte))
            continue;
        uint64_t phys = pte_phys(*pte);
        *pte = 0;
        arch_tlb_flush_page(va);
        b->changed = true;
        if (v->flags & VMA_PHYS)
            continue;
        /* every page is counted (private ones may be shared copy-on-write since a fork) */
        b->pages[b->n++] = phys | BATCH_SHARED;
        if (b->n == FREE_BATCH)
            batch_flush(mm, b);
    }
}

/* Frees pages and splits/removes VMAs in [start, end). Lock held. */
static void unmap_locked(struct mm *mm, uint64_t start, uint64_t end) {
    struct free_batch batch;
    batch.n = 0;
    batch.changed = false;
    /* Only pages inside VMAs can be mapped, so walk those, not the range. */
    for (struct vma *v = mm->vmas; v; v = v->next) {
        uint64_t s = v->start > start ? v->start : start, e = v->end < end ? v->end : end;
        if (s < e)
            zap_locked(mm, v, s, e, &batch);
    }
    if (batch.changed)
        batch_flush(mm, &batch);
    struct vma **pp = &mm->vmas;
    while (*pp) {
        struct vma *v = *pp;
        if (v->end <= start || v->start >= end) {
            pp = &v->next;
            continue;
        }
        if (v->start < start && v->end > end) { /* hole in the middle: split */
            struct vma *tail = vma_dup(v);
            KASSERT(tail);
            vma_set_start(tail, end);
            tail->next = v->next;
            v->end = start;
            v->next = tail;
            return;
        }
        if (v->start < start) {
            v->end = start;
            pp = &v->next;
        } else if (v->end > end) {
            vma_set_start(v, end);
            pp = &v->next;
        } else {
            *pp = v->next;
            vma_free(v);
        }
    }
}

/* Inserts `nv` (start..end set), replacing whatever was mapped there. Lock held. */
static void insert_locked(struct mm *mm, struct vma *nv) {
    unmap_locked(mm, nv->start, nv->end);
    struct vma **pp = &mm->vmas;
    while (*pp && (*pp)->start < nv->start)
        pp = &(*pp)->next;
    nv->next = *pp;
    *pp = nv;
}

int mm_map(struct mm *mm, uint64_t start, uint64_t end, uint32_t prot, uint32_t flags) {
    if (start >= end || end > USER_TOP || (start & 0xfff) || (end & 0xfff))
        return -EINVAL;
    struct vma *nv = kmalloc(sizeof *nv);
    if (!nv)
        return -ENOMEM;
    *nv = (struct vma){.start = start, .end = end, .prot = prot, .flags = flags};
    uint64_t f = spin_lock_irqsave(&mm->lock);
    insert_locked(mm, nv);
    spin_unlock_irqrestore(&mm->lock, f);
    return 0;
}

int mm_map_phys(struct mm *mm, uint64_t start, uint64_t end, uint32_t prot, uint64_t phys) {
    int r = mm_map(mm, start, end, prot, VMA_PHYS);
    if (r < 0)
        return r;
    uint64_t f = spin_lock_irqsave(&mm->lock);
    for (uint64_t va = start; va < end; va += PAGE_SIZE, phys += PAGE_SIZE) {
        uint64_t *pte = pte_slot(mm, va, true);
        if (!pte) {
            r = -ENOMEM;
            break;
        }
        *pte = pte_make(phys, pte_flags_for(prot) | PG_WC);
    }
    spin_unlock_irqrestore(&mm->lock, f);
    return r;
}

int mm_unmap(struct mm *mm, uint64_t start, uint64_t end) {
    if ((start & 0xfff) || end > USER_TOP || start > end)
        return -EINVAL;
    uint64_t f = spin_lock_irqsave(&mm->lock);
    unmap_locked(mm, start, ALIGN_UP(end, PAGE_SIZE));
    spin_unlock_irqrestore(&mm->lock, f);
    return 0;
}

int mm_protect(struct mm *mm, uint64_t start, uint64_t end, uint32_t prot) {
    if ((start & 0xfff) || end > USER_TOP || start > end)
        return -EINVAL;
    end = ALIGN_UP(end, PAGE_SIZE);
    uint64_t f = spin_lock_irqsave(&mm->lock);
    /* Split VMAs at the boundaries, then retag the ones inside. */
    for (struct vma *v = mm->vmas; v; v = v->next) {
        if (v->end <= start || v->start >= end)
            continue;
        if (v->start < start) {
            struct vma *tail = vma_dup(v);
            KASSERT(tail);
            vma_set_start(tail, start);
            tail->next = v->next;
            v->end = start;
            v->next = tail;
            continue; /* the loop visits `tail` next */
        }
        if (v->end > end) {
            struct vma *tail = vma_dup(v);
            KASSERT(tail);
            vma_set_start(tail, end);
            tail->next = v->next;
            v->end = end;
            v->next = tail;
        }
        v->prot = prot;
    }
    bool changed = false;
    uint64_t span;
    for (uint64_t va = start; va < end; va = walk_next(va, span)) {
        uint64_t *pte = pte_find(mm, va, &span);
        if (pte && pte_present(*pte)) {
            uint64_t flags = pte_flags_for(prot) | (pte_flags(*pte) & (PG_WC | PG_COW));
            if (flags & PG_COW)
                flags &= ~PG_WRITE; /* still shared: the first write copies it */
            *pte = pte_make(pte_phys(*pte), flags);
            arch_tlb_flush_page(va);
            changed = true;
        }
    }
    if (changed)
        tlb_shootdown(mm);
    spin_unlock_irqrestore(&mm->lock, f);
    return 0;
}

#define MMAP_LIMIT (USER_STACK_TOP - USER_STACK_SIZE - PAGE_SIZE)

static uint64_t find_free_locked(struct mm *mm, uint64_t len, uint64_t hint) {
    uint64_t cand = hint ? ALIGN_UP(hint, PAGE_SIZE) : mm->mmap_hint;
    for (int pass = 0; pass < 2; pass++) {
        bool moved = true;
        while (moved) {
            moved = false;
            if (cand + len > MMAP_LIMIT)
                break;
            for (struct vma *v = mm->vmas; v; v = v->next) {
                if (cand < v->end && cand + len > v->start) {
                    cand = v->end;
                    moved = true;
                }
            }
        }
        if (!moved && cand + len <= MMAP_LIMIT)
            return cand;
        cand = USER_MMAP_BASE;
    }
    return 0;
}

uint64_t mm_find_free(struct mm *mm, uint64_t len, uint64_t hint) {
    uint64_t f = spin_lock_irqsave(&mm->lock);
    uint64_t addr = find_free_locked(mm, ALIGN_UP(len, PAGE_SIZE), hint);
    spin_unlock_irqrestore(&mm->lock, f);
    return addr;
}

/* Finding a hole and mapping it must be one step under the lock: two
 * threads calling mmap at once would otherwise be handed the same range. */
uint64_t mm_map_free(struct mm *mm, uint64_t len, uint64_t hint, uint32_t prot, uint32_t flags) {
    struct vma *nv = kmalloc(sizeof *nv);
    if (!nv)
        return 0;
    len = ALIGN_UP(len, PAGE_SIZE);
    uint64_t f = spin_lock_irqsave(&mm->lock);
    uint64_t addr = find_free_locked(mm, len, hint);
    if (addr) {
        *nv = (struct vma){.start = addr, .end = addr + len, .prot = prot, .flags = flags};
        insert_locked(mm, nv);
    }
    spin_unlock_irqrestore(&mm->lock, f);
    if (!addr)
        kfree(nv);
    return addr;
}

int64_t mm_map_shared(struct mm *mm, uint64_t addr, uint64_t len, uint32_t prot, bool fixed, struct pageobj *obj,
                      uint64_t pgoff) {
    len = ALIGN_UP(len, PAGE_SIZE);
    if (!len || len > USER_TOP || (fixed && ((addr & 0xfff) || addr > USER_TOP - len)))
        return -EINVAL;
    struct vma *nv = kmalloc(sizeof *nv);
    if (!nv)
        return -ENOMEM;
    uint64_t f = spin_lock_irqsave(&mm->lock);
    if (!fixed)
        addr = find_free_locked(mm, len, addr);
    bool ok = fixed || addr;
    if (ok) {
        pageobj_ref(obj);
        *nv = (struct vma){.start = addr, .end = addr + len, .prot = prot, .flags = VMA_SHARED, .obj = obj,
                           .pgoff = pgoff};
        insert_locked(mm, nv);
    }
    spin_unlock_irqrestore(&mm->lock, f);
    if (!ok) {
        kfree(nv);
        return -ENOMEM;
    }
    return (int64_t)addr;
}

/* True if nothing is mapped in [start, end) and it stays below the stack. */
static bool range_free_locked(struct mm *mm, uint64_t start, uint64_t end) {
    if (end < start || end > MMAP_LIMIT)
        return false;
    for (struct vma *v = mm->vmas; v; v = v->next)
        if (v->start < end && v->end > start)
            return false;
    return true;
}

/* Moves [old, old+old_len) of `v` to a fresh range of new_len bytes. The
 * page-table entries move as they are: the pages, and for shared mappings
 * their references, simply change address. Lock held. */
static int64_t move_locked(struct mm *mm, struct vma *v, uint64_t old, uint64_t old_len, uint64_t new_len) {
    uint64_t dst = find_free_locked(mm, new_len, 0);
    if (!dst)
        return -ENOMEM;
    /* Create the destination page tables first, so running out of memory
     * cannot leave the move half done. */
    for (uint64_t o = 0; o < old_len; o += PAGE_SIZE) {
        uint64_t *spte = pte_slot(mm, old + o, false);
        if (spte && pte_present(*spte) && !pte_slot(mm, dst + o, true))
            return -ENOMEM;
    }
    struct vma *nv = vma_dup(v);
    if (!nv)
        return -ENOMEM;
    nv->pgoff = v->pgoff + (old - v->start) / PAGE_SIZE;
    nv->start = dst;
    nv->end = dst + new_len;
    bool moved = false;
    for (uint64_t o = 0; o < old_len; o += PAGE_SIZE) {
        uint64_t *spte = pte_slot(mm, old + o, false);
        if (!spte || !pte_present(*spte))
            continue;
        *pte_slot(mm, dst + o, false) = *spte;
        *spte = 0;
        arch_tlb_flush_page(old + o);
        moved = true;
    }
    /* Another thread may still reach the pages through the old addresses
     * until its TLB is flushed; the pages stay alive, so flushing once
     * afterwards is enough. The old range now has no PTEs, so unmapping it
     * only trims or removes its VMA. */
    if (moved)
        tlb_shootdown(mm);
    unmap_locked(mm, old, old + old_len);
    insert_locked(mm, nv);
    return (int64_t)dst;
}

int64_t mm_remap(struct mm *mm, uint64_t old_addr, uint64_t old_len, uint64_t new_len, bool may_move) {
    old_len = ALIGN_UP(old_len, PAGE_SIZE);
    new_len = ALIGN_UP(new_len, PAGE_SIZE);
    if ((old_addr & 0xfff) || !new_len || old_addr >= USER_TOP || old_len > USER_TOP - old_addr ||
        new_len > USER_TOP)
        return -EINVAL;
    uint64_t f = spin_lock_irqsave(&mm->lock);
    int64_t r;
    struct vma *v = mm_find_vma(mm, old_addr);
    if (new_len <= old_len) {
        unmap_locked(mm, old_addr + new_len, old_addr + old_len);
        r = (int64_t)old_addr;
    } else if (!old_len) {
        r = -EINVAL; /* Linux's "duplicate a shared mapping" form: not supported */
    } else if (!v || old_addr + old_len > v->end) {
        r = -EFAULT;
    } else if (!(v->flags & (VMA_ANON | VMA_SHARED)) || (v->flags & (VMA_PHYS | VMA_GROWSDOWN))) {
        /* Private file copies would need the file read in, device memory
         * cannot grow: libc falls back to mmap + copy on ENOMEM. */
        r = -ENOMEM;
    } else if (old_addr + old_len == v->end && range_free_locked(mm, v->end, old_addr + new_len)) {
        v->end = old_addr + new_len; /* new pages demand-fault like the rest */
        r = (int64_t)old_addr;
    } else if (may_move) {
        r = move_locked(mm, v, old_addr, old_len, new_len);
    } else {
        r = -ENOMEM;
    }
    spin_unlock_irqrestore(&mm->lock, f);
    return r;
}

int mm_dontneed(struct mm *mm, uint64_t start, uint64_t end) {
    if ((start & 0xfff) || end > USER_TOP || start > end)
        return -EINVAL;
    end = ALIGN_UP(end, PAGE_SIZE);
    struct free_batch batch;
    batch.n = 0;
    batch.changed = false;
    uint64_t f = spin_lock_irqsave(&mm->lock);
    for (struct vma *v = mm->vmas; v; v = v->next) {
        /* Private file mappings would have to go back to the file's
         * contents, not zeros: leave those (and device memory) alone. */
        if (!(v->flags & (VMA_ANON | VMA_SHARED | VMA_GROWSDOWN)) || (v->flags & VMA_PHYS))
            continue;
        uint64_t s = v->start > start ? v->start : start, e = v->end < end ? v->end : end;
        if (s < e)
            zap_locked(mm, v, s, e, &batch);
    }
    if (batch.changed)
        batch_flush(mm, &batch);
    spin_unlock_irqrestore(&mm->lock, f);
    return 0;
}

/* Makes `va` (inside `v`) present: a zeroed page for private memory, the
 * object's page for shared mappings. Lock held. */
static uint64_t *fault_in(struct mm *mm, struct vma *v, uint64_t va) {
    uint64_t *pte = pte_slot(mm, va, true);
    if (!pte)
        return NULL;
    if (pte_present(*pte))
        return pte;
    uint64_t p;
    if (v->flags & VMA_SHARED) {
        /* 0 past the object's end: like Linux's SIGBUS, the access fails. */
        p = pageobj_map_page(v->obj, v->pgoff + (va - v->start) / PAGE_SIZE);
    } else if (v->flags & VMA_PHYS) {
        p = 0; /* device mappings are filled in completely at mmap time */
    } else {
        p = page_alloc_shared(); /* counted: a fork shares it copy-on-write */
        if (p)
            memset(phys_to_virt(p), 0, PAGE_SIZE);
    }
    if (!p)
        return NULL;
    *pte = pte_make(p, pte_flags_for(v->prot));
    return pte;
}

bool mm_handle_fault(struct mm *mm, uint64_t addr, bool write, bool exec) {
    if (!mm || addr >= USER_TOP)
        return false;
    uint64_t f = spin_lock_irqsave(&mm->lock);
    struct vma *v = mm_find_vma(mm, addr);
    bool ok = v && (!write || (v->prot & PROT_WRITE)) && (!exec || (v->prot & PROT_EXEC)) &&
              (v->prot & (PROT_READ | PROT_WRITE | PROT_EXEC));
    if (ok) {
        uint64_t *pte = pte_slot(mm, ALIGN_DOWN(addr, PAGE_SIZE), false);
        if (pte && pte_present(*pte) && write && (pte_flags(*pte) & PG_COW)) {
            ok = break_cow(mm, v, ALIGN_DOWN(addr, PAGE_SIZE), pte);
        } else if (pte && pte_present(*pte)) {
            /* Present but faulted: either a real protection violation, or
             * this CPU used a stale TLB entry while another thread's mprotect
             * was widening the page (before its shootdown reached us). In
             * the latter case the PTE already allows the access: retry. */
            uint64_t pg = pte_flags(*pte);
            ok = (!write || (pg & PG_WRITE)) && (!exec || (pg & PG_EXEC));
            if (ok)
                arch_tlb_flush_page(ALIGN_DOWN(addr, PAGE_SIZE));
        } else
            ok = fault_in(mm, v, ALIGN_DOWN(addr, PAGE_SIZE)) != NULL;
    }
    spin_unlock_irqrestore(&mm->lock, f);
    return ok;
}

static int copy_pages(struct mm *mm, uint64_t dst, const void *src, size_t len) {
    const uint8_t *s = src;
    while (len) {
        uint64_t page = ALIGN_DOWN(dst, PAGE_SIZE), off = dst - page;
        size_t n = PAGE_SIZE - off < len ? PAGE_SIZE - off : len;
        struct vma *v = mm_find_vma(mm, page);
        if (!v)
            return -EFAULT;
        uint64_t *pte = fault_in(mm, v, page);
        if (!pte)
            return -ENOMEM;
        if ((pte_flags(*pte) & PG_COW) && !break_cow(mm, v, page, pte)) /* written below, through the kernel's view */
            return -ENOMEM;
        uint8_t *kva = (uint8_t *)phys_to_virt(pte_phys(*pte)) + off;
        if (s)
            memcpy(kva, s, n);
        else
            memset(kva, 0, n);
        dst += n;
        len -= n;
        if (s)
            s += n;
    }
    return 0;
}

int mm_copy_out(struct mm *mm, uint64_t dst, const void *src, size_t len) {
    uint64_t f = spin_lock_irqsave(&mm->lock);
    int r = copy_pages(mm, dst, src, len);
    spin_unlock_irqrestore(&mm->lock, f);
    return r;
}

int mm_zero(struct mm *mm, uint64_t dst, size_t len) { return mm_copy_out(mm, dst, NULL, len); }

struct mm *mm_clone(struct mm *src) {
    struct mm *mm = mm_create();
    if (!mm)
        return NULL;
    uint64_t f = spin_lock_irqsave(&src->lock);
    mm->brk_start = src->brk_start;
    mm->brk = src->brk;
    mm->mmap_hint = src->mmap_hint;
    struct vma **tail = &mm->vmas;
    bool cow = false;
    for (struct vma *v = src->vmas; v; v = v->next) {
        struct vma *nv = vma_dup(v);
        if (!nv)
            goto fail;
        *tail = nv;
        tail = &nv->next;
        uint64_t span;
        for (uint64_t va = v->start; va < v->end; va = walk_next(va, span)) {
            uint64_t *spte = pte_find(src, va, &span);
            if (!spte || !pte_present(*spte))
                continue;
            uint64_t *dpte = pte_slot(mm, va, true);
            if (!dpte)
                goto fail;
            if (v->flags & VMA_PHYS) {
                *dpte = *spte; /* device memory is shared, not copied */
                continue;
            }
            /* Shared mappings: both map the object's page. Private memory:
             * both map the same page read-only, copy-on-write; the first
             * to write gets its own copy (break_cow). */
            page_ref(pte_phys(*spte));
            if (!(v->flags & VMA_SHARED)) {
                *spte = pte_make(pte_phys(*spte), (pte_flags(*spte) & ~PG_WRITE) | PG_COW);
                cow = true;
            }
            *dpte = *spte;
        }
    }
    /* the parent (this CPU too) must not keep writing through old writable TLB entries */
    if (cow) {
        tlb_shootdown(src);
        if (arch_current_root() == virt_to_phys_hhdm(src->pgd))
            arch_tlb_flush_all();
    }
    spin_unlock_irqrestore(&src->lock, f);
    return mm;
fail:
    if (cow) {
        tlb_shootdown(src);
        if (arch_current_root() == virt_to_phys_hhdm(src->pgd))
            arch_tlb_flush_all();
    }
    spin_unlock_irqrestore(&src->lock, f);
    mm_put(mm);
    return NULL;
}

static void free_tables(uint64_t *table, int level) {
    for (int i = 0; i < (level == 4 ? 256 : 512); i++) {
        if (!pte_present(table[i]))
            continue;
        if (level > 2)
            free_tables(phys_to_virt(pte_phys(table[i])), level - 1);
        if (level > 1)
            pmm_free(pte_phys(table[i]), 1);
    }
}

void mm_put(struct mm *mm) {
    if (__atomic_sub_fetch(&mm->refcount, 1, __ATOMIC_ACQ_REL) != 0)
        return;
    KASSERT(arch_current_root() != virt_to_phys_hhdm(mm->pgd));
    uint64_t f = spin_lock_irqsave(&mm->lock);
    unmap_locked(mm, 0, USER_TOP);
    spin_unlock_irqrestore(&mm->lock, f);
    /* Leaf pages are gone; free the page-table pages of the lower half. */
    free_tables(mm->pgd, 4);
    pmm_free(virt_to_phys_hhdm(mm->pgd), 1);
    kfree(mm);
}

/* ---- access to the current process's memory from system calls ----
 * The copies run in usercopy.S. They simply touch user memory: a missing
 * page is demand-faulted in as for the program itself, and a fault that
 * cannot be resolved (unmapped, forbidden, or unmapped by another thread
 * mid-copy) lands on the routine's fixup, which returns -EFAULT. So all we
 * check here is that the range lies in the user half. */

int __copy_user(void *dst, const void *src, size_t len);
int64_t __strncpy_user(char *dst, const char *src, size_t size);
int __cmpxchg_user32(uint64_t uaddr, uint32_t expected, uint32_t desired, uint32_t *cur);

struct ex_entry {
    uint64_t insn, fixup;
};
extern const struct ex_entry __ex_table_start[], __ex_table_end[];

uint64_t exception_fixup(uint64_t rip) {
    for (const struct ex_entry *e = __ex_table_start; e < __ex_table_end; e++)
        if (e->insn == rip)
            return e->fixup;
    return 0;
}

static bool user_range_ok(uint64_t addr, size_t len) { return addr < USER_TOP && len <= USER_TOP - addr; }

int copy_from_user(void *dst, uint64_t src, size_t len) {
    if (!len)
        return 0;
    if (!user_range_ok(src, len))
        return -EFAULT;
    return __copy_user(dst, (const void *)src, len);
}

int copy_to_user(uint64_t dst, const void *src, size_t len) {
    if (!len)
        return 0;
    if (!user_range_ok(dst, len))
        return -EFAULT;
    return __copy_user((void *)dst, src, len);
}

int64_t strncpy_from_user(char *dst, uint64_t src, size_t size) {
    if (src >= USER_TOP)
        return -EFAULT;
    size_t max = USER_TOP - src < size ? USER_TOP - src : size;
    int64_t n = __strncpy_user(dst, (const char *)src, max);
    if (n < 0)
        return n;
    if ((size_t)n == max) /* no NUL within reach */
        return max < size ? -EFAULT : -ENAMETOOLONG;
    return n;
}

int futex_cmpxchg_user(uint64_t uaddr, uint32_t expected, uint32_t desired, uint32_t *cur) {
    if ((uaddr & 3) || !user_range_ok(uaddr, 4))
        return -EFAULT;
    return __cmpxchg_user32(uaddr, expected, desired, cur);
}
