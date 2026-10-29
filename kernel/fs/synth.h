#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <kernel/sbuf.h>
#include <kernel/vfs.h>

/* Synthetic file systems (/proc, /sys): a tree described by static tables,
 * whose nodes are vnodes made on demand by lookup and whose files are
 * generated when read. Nothing is stored: a node is its table entry plus
 * the ids taken from the path (/proc/<pid>/task/<tid>/fd/<fd>, or a CPU and
 * cache index under /sys). */

struct snode;

struct sent {
    const char *name;
    enum vnode_type type; /* VN_REG, VN_DIR or VN_LNK */
    uint16_t mode;
    /* files */
    int (*show)(struct snode *n, struct sbuf *b);
    int64_t (*store)(struct snode *n, const char *buf, size_t len); /* writable files */
    /* links: readlink text (returns its length), and for magic links the
     * object itself */
    int (*link)(struct snode *n, char *buf, size_t size);
    int (*follow)(struct snode *n, struct path *out);
    /* directories: fixed entries (ending with a NULL name), then dynamic
     * ones, each of kind `dyn_ent` with its id stored in id[dyn_slot] */
    const struct sent *children;
    const struct sent *dyn_ent;
    int dyn_slot;
    /* The i-th dynamic entry: fills name and id, returns 1; 0 at the end. */
    int (*dyn_nth)(struct snode *dir, uint64_t i, char *name, size_t size, int *id);
    /* Parses `name`; returns 0 and the id if such an entry exists. */
    int (*dyn_find)(struct snode *dir, const char *name, int *id);
    /* Optional: the directory's link count when it isn't 2 (a task
     * directory has one more per thread; Chromium counts threads so). */
    int (*links)(struct snode *n);
};

struct snode {
    const struct sent *ent;
    int id[3];
};

/* The root vnode of a tree (referenced). */
struct vnode *synth_root(const struct sent *root);
