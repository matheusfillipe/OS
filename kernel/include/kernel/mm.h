#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <kernel/spinlock.h>

/* Physical frame allocator (bitmap, one bit per 4 KiB frame). Returns
 * physical addresses; 0 means out of memory (frame 0 is never handed out). */
void pmm_init(void);
uint64_t pmm_alloc(size_t pages);
void pmm_free(uint64_t phys, size_t pages);
uint64_t pmm_free_pages(void);
uint64_t pmm_total_pages(void);

/* Reference-counted single pages, for memory with several owners at once
 * (a file's page that is also mapped by processes). page_alloc_shared
 * returns a page (not zeroed) holding one reference; the last page_unref
 * frees it. Never mix with pmm_free on the same page. */
uint64_t page_alloc_shared(void);
void page_ref(uint64_t phys);
void page_unref(uint64_t phys);
/* How many references a page has (copy-on-write: one means it is ours alone). */
uint32_t page_refcount(uint64_t phys);
void page_pin(uint64_t phys); /* boot-module memory that is never freed */

/* Page-backed storage for in-memory files (ramfs, memfd) and shared
 * anonymous memory: an array of single pages, each allocated (zero-filled)
 * on first write or first mapping fault. Holes read as zeros. The object
 * holds one reference on each of its pages and every page-table entry
 * mapping one holds another, so a mapped page outlives truncation and the
 * file itself. Buffers passed in are kernel memory. */
struct pageobj {
    int refcount;
    spinlock_t lock;
    uint64_t size;   /* bytes */
    uint64_t nslots; /* entries in `pages` */
    uint64_t *pages; /* physical address per page index, 0 = hole */
};
struct pageobj *pageobj_create(void);
void pageobj_ref(struct pageobj *o);
void pageobj_put(struct pageobj *o);
uint64_t pageobj_size(struct pageobj *o);
int64_t pageobj_read(struct pageobj *o, void *buf, size_t len, uint64_t off);
/* Writes extend the size; returns bytes written or -ENOMEM. */
int64_t pageobj_write(struct pageobj *o, const void *buf, size_t len, uint64_t off);
int pageobj_truncate(struct pageobj *o, uint64_t size);
/* The page holding page index `idx`, allocated if it is a hole, with a new
 * reference for the caller (a page-table entry). 0 if `idx` lies wholly
 * past the end of the object, or memory ran out. */
uint64_t pageobj_map_page(struct pageobj *o, uint64_t idx);
struct pageobj *pageobj_adopt(uint64_t phys, uint64_t size); /* pinned memory as a file */

/* Generic page mapping flags; <asm/pgtable.h> turns them into entries.
 * Leaf mappings are always present and readable. */
#define PG_WRITE    (1ULL << 0)
#define PG_USER     (1ULL << 1)
#define PG_EXEC     (1ULL << 2)
#define PG_GLOBAL   (1ULL << 3)
#define PG_UNCACHED (1ULL << 4)
#define PG_WC       (1ULL << 5)
#define PG_COW      (1ULL << 6) /* shared since fork; a write copies it */

#include <asm/pgtable.h>

/* Builds the kernel's own page tables (kernel image with W^X permissions +
 * direct map of physical memory) and switches to them. */
void vmm_init(void);
bool vmm_map(uint64_t *pgd, uint64_t virt, uint64_t phys, uint64_t flags);
bool vmm_map_huge(uint64_t *pgd, uint64_t virt, uint64_t phys, uint64_t flags);
uint64_t vmm_translate(uint64_t *pgd, uint64_t virt);
uint64_t *vmm_kernel_pgd(void);
/* Makes [phys, phys+len) reachable through the direct map (for firmware
 * tables and device registers outside the memory map). Pages already mapped
 * are left alone. `uncached` selects strong-uncacheable for device MMIO. */
void vmm_map_mmio(uint64_t phys, uint64_t len, bool uncached);

/* Kernel heap. */
void heap_init(void);
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void kfree(void *p);
