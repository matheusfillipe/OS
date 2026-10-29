#include <kernel/arch.h>
#include <kernel/blkdev.h>
#include <kernel/cpu.h>
#include <kernel/lib.h>
#include <kernel/vfs.h>
#include <linux/abi.h>
#include "synth.h"

/* /sys: the small part of Linux's sysfs that user space reads to size
 * itself: the CPU list and topology (glibc's get_nprocs_conf, ONNX Runtime
 * and other users of the cpuinfo library), cache geometry, disks, and the
 * transparent hugepage policy (jemalloc, glibc tunables).
 * Node ids: id[0] = CPU or disk index, id[1] = cache index. */

static void cpu_range(struct sbuf *b) {
    if (cpu_count > 1)
        sb_printf(b, "0-%u\n", cpu_count - 1);
    else
        sb_printf(b, "0\n");
}

static int show_cpu_range(struct snode *n, struct sbuf *b) {
    (void)n;
    cpu_range(b);
    return 0;
}

static int show_empty_line(struct snode *n, struct sbuf *b) {
    (void)n;
    sb_put(b, "\n", 1);
    return 0;
}

static int show_kernel_max(struct snode *n, struct sbuf *b) {
    (void)n;
    sb_printf(b, "%d\n", MAX_CPUS - 1);
    return 0;
}

static int show_one(struct snode *n, struct sbuf *b) {
    (void)n;
    sb_printf(b, "1\n");
    return 0;
}

static int show_zero(struct snode *n, struct sbuf *b) {
    (void)n;
    sb_printf(b, "0\n");
    return 0;
}

static int show_cpu_id(struct snode *n, struct sbuf *b) {
    sb_printf(b, "%d\n", n->id[0]);
    return 0;
}

static void mask_of(struct sbuf *b, uint32_t first, uint32_t last) {
    /* comma-separated 32-bit groups, most significant first */
    int groups = (int)((cpu_count + 31) / 32);
    for (int g = groups - 1; g >= 0; g--) {
        uint32_t w = 0;
        for (uint32_t i = 0; i < 32; i++) {
            uint32_t c = (uint32_t)g * 32 + i;
            if (c >= first && c <= last)
                w |= 1u << i;
        }
        sb_printf(b, "%08x%s", w, g ? "," : "\n");
    }
}

static int show_self_list(struct snode *n, struct sbuf *b) {
    sb_printf(b, "%d\n", n->id[0]);
    return 0;
}

static int show_self_mask(struct snode *n, struct sbuf *b) {
    mask_of(b, (uint32_t)n->id[0], (uint32_t)n->id[0]);
    return 0;
}

static int show_all_mask(struct snode *n, struct sbuf *b) {
    (void)n;
    mask_of(b, 0, cpu_count - 1);
    return 0;
}

static int show_freq(struct snode *n, struct sbuf *b) {
    (void)n;
    uint32_t mhz = arch_base_mhz();
    sb_printf(b, "%u\n", (mhz ? mhz : 2000) * 1000);
    return 0;
}

/* ---- caches ---- */

static int cache_nth(struct snode *dir, uint64_t i, char *name, size_t size, int *id) {
    (void)dir;
    struct arch_cache ci;
    if (i > 16 || !arch_cache_info((int)i, &ci))
        return 0;
    ksnprintf(name, size, "index%lu", i);
    *id = (int)i;
    return 1;
}

static int cache_find(struct snode *dir, const char *name, int *id) {
    (void)dir;
    if (strncmp(name, "index", 5) || name[5] < '0' || name[5] > '9' || (name[6] && name[7]))
        return -ENOENT;
    int i = name[5] - '0';
    if (name[6]) {
        if (name[6] < '0' || name[6] > '9')
            return -ENOENT;
        i = i * 10 + (name[6] - '0');
    }
    struct arch_cache ci;
    if (!arch_cache_info(i, &ci))
        return -ENOENT;
    *id = i;
    return 0;
}

enum cache_field { CF_LEVEL, CF_TYPE, CF_SIZE, CF_LINE, CF_WAYS, CF_SETS, CF_PART, CF_SHARED, CF_SHARED_MAP };

static int show_cache(struct snode *n, struct sbuf *b, enum cache_field f) {
    struct arch_cache ci;
    if (!arch_cache_info(n->id[1], &ci))
        return -ENOENT;
    uint32_t ways = ci.ways, parts = ci.parts, line = ci.line, sets = ci.sets, sharing = ci.sharing;
    switch (f) {
    case CF_LEVEL:
        sb_printf(b, "%u\n", ci.level);
        break;
    case CF_TYPE: {
        uint32_t t = ci.type;
        sb_printf(b, "%s\n", t == 1 ? "Data" : t == 2 ? "Instruction" : "Unified");
        break;
    }
    case CF_SIZE:
        sb_printf(b, "%uK\n", ways * parts * line * sets / 1024);
        break;
    case CF_LINE:
        sb_printf(b, "%u\n", line);
        break;
    case CF_WAYS:
        sb_printf(b, "%u\n", ways);
        break;
    case CF_SETS:
        sb_printf(b, "%u\n", sets);
        break;
    case CF_PART:
        sb_printf(b, "%u\n", parts);
        break;
    case CF_SHARED:
    case CF_SHARED_MAP: {
        /* Logical CPUs sharing it, in aligned groups of `sharing`. */
        uint32_t group = sharing >= cpu_count ? cpu_count : sharing;
        uint32_t first = (uint32_t)n->id[0] / group * group, last = first + group - 1;
        if (last >= cpu_count)
            last = cpu_count - 1;
        if (f == CF_SHARED_MAP)
            mask_of(b, first, last);
        else if (first == last)
            sb_printf(b, "%u\n", first);
        else
            sb_printf(b, "%u-%u\n", first, last);
        break;
    }
    }
    return 0;
}

#define CACHE_SHOW(nm, f)                                                                                \
    static int show_cache_##nm(struct snode *n, struct sbuf *b) { return show_cache(n, b, f); }
CACHE_SHOW(level, CF_LEVEL)
CACHE_SHOW(type, CF_TYPE)
CACHE_SHOW(size, CF_SIZE)
CACHE_SHOW(line, CF_LINE)
CACHE_SHOW(ways, CF_WAYS)
CACHE_SHOW(sets, CF_SETS)
CACHE_SHOW(parts, CF_PART)
CACHE_SHOW(shared, CF_SHARED)
CACHE_SHOW(shared_map, CF_SHARED_MAP)

static int show_cache_id(struct snode *n, struct sbuf *b) {
    struct arch_cache ci;
    if (!arch_cache_info(n->id[1], &ci))
        return -ENOENT;
    uint32_t group = ci.sharing >= cpu_count ? cpu_count : ci.sharing;
    sb_printf(b, "%u\n", (uint32_t)n->id[0] / (group ? group : 1));
    return 0;
}

#define RO(nm, fn) {.name = nm, .type = VN_REG, .mode = 0444, .show = fn}

static const struct sent cache_children[] = {
    RO("coherency_line_size", show_cache_line),
    RO("id", show_cache_id),
    RO("level", show_cache_level),
    RO("number_of_sets", show_cache_sets),
    RO("physical_line_partition", show_cache_parts),
    RO("shared_cpu_list", show_cache_shared),
    RO("shared_cpu_map", show_cache_shared_map),
    RO("size", show_cache_size),
    RO("type", show_cache_type),
    RO("ways_of_associativity", show_cache_ways),
    {0},
};
static const struct sent cache_ent = {.name = "index", .type = VN_DIR, .mode = 0755, .children = cache_children};

static const struct sent topology_children[] = {
    RO("cluster_cpus", show_self_mask),
    RO("cluster_cpus_list", show_self_list),
    RO("cluster_id", show_cpu_id),
    RO("core_cpus", show_self_mask),
    RO("core_cpus_list", show_self_list),
    RO("core_id", show_cpu_id),
    RO("core_siblings", show_all_mask),
    RO("core_siblings_list", show_cpu_range),
    RO("die_cpus", show_all_mask),
    RO("die_cpus_list", show_cpu_range),
    RO("die_id", show_zero),
    RO("package_cpus", show_all_mask),
    RO("package_cpus_list", show_cpu_range),
    RO("physical_package_id", show_zero),
    RO("thread_siblings", show_self_mask),
    RO("thread_siblings_list", show_self_list),
    {0},
};

static const struct sent cpufreq_children[] = {
    RO("base_frequency", show_freq),
    RO("cpuinfo_max_freq", show_freq),
    RO("cpuinfo_min_freq", show_freq),
    RO("scaling_cur_freq", show_freq),
    RO("scaling_max_freq", show_freq),
    {0},
};

static const struct sent cpu_children[] = {
    {.name = "cache", .type = VN_DIR, .mode = 0755, .dyn_ent = &cache_ent, .dyn_slot = 1, .dyn_nth = cache_nth,
     .dyn_find = cache_find},
    {.name = "cpufreq", .type = VN_DIR, .mode = 0755, .children = cpufreq_children},
    RO("online", show_one),
    {.name = "topology", .type = VN_DIR, .mode = 0755, .children = topology_children},
    {0},
};
static const struct sent cpu_ent = {.name = "cpu", .type = VN_DIR, .mode = 0755, .children = cpu_children};

static int cpu_nth(struct snode *dir, uint64_t i, char *name, size_t size, int *id) {
    (void)dir;
    if (i >= cpu_count)
        return 0;
    ksnprintf(name, size, "cpu%lu", i);
    *id = (int)i;
    return 1;
}

static int parse_small(const char *s, int *out) {
    int v = 0;
    if (!*s || (s[0] == '0' && s[1]))
        return -ENOENT;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > 100000)
            return -ENOENT;
        v = v * 10 + (*s - '0');
    }
    *out = v;
    return 0;
}

static int cpu_find(struct snode *dir, const char *name, int *id) {
    (void)dir;
    int i;
    if (strncmp(name, "cpu", 3) || parse_small(name + 3, &i) < 0 || (uint32_t)i >= cpu_count)
        return -ENOENT;
    *id = i;
    return 0;
}

static const struct sent cpus_children[] = {
    RO("kernel_max", show_kernel_max),
    RO("offline", show_empty_line),
    RO("online", show_cpu_range),
    RO("possible", show_cpu_range),
    RO("present", show_cpu_range),
    {0},
};

/* ---- disks ---- */

static struct blkdev *disk(int index) {
    char name[8];
    ksnprintf(name, sizeof name, "sd%c", 'a' + index);
    return index >= 0 && index < 26 ? blkdev_find(name) : NULL;
}

static int disk_nth(struct snode *dir, uint64_t i, char *name, size_t size, int *id) {
    (void)dir;
    struct blkdev *d = i < 26 ? disk((int)i) : NULL;
    if (!d)
        return 0;
    ksnprintf(name, size, "%s", d->name);
    *id = (int)i;
    return 1;
}

static int disk_find(struct snode *dir, const char *name, int *id) {
    (void)dir;
    if (name[0] != 's' || name[1] != 'd' || name[2] < 'a' || name[2] > 'z' || name[3])
        return -ENOENT;
    if (!disk(name[2] - 'a'))
        return -ENOENT;
    *id = name[2] - 'a';
    return 0;
}

static int show_disk_size(struct snode *n, struct sbuf *b) {
    struct blkdev *d = disk(n->id[0]);
    if (!d)
        return -ENOENT;
    sb_printf(b, "%lu\n", d->sectors * d->sector_size / 512); /* always 512-byte units */
    return 0;
}

static int show_disk_dev(struct snode *n, struct sbuf *b) {
    sb_printf(b, "8:%d\n", n->id[0] * 16);
    return 0;
}

static int show_sector_size(struct snode *n, struct sbuf *b) {
    struct blkdev *d = disk(n->id[0]);
    if (!d)
        return -ENOENT;
    sb_printf(b, "%u\n", d->sector_size);
    return 0;
}

static int show_max_sectors(struct snode *n, struct sbuf *b) {
    (void)n;
    sb_printf(b, "1280\n");
    return 0;
}

static int show_uevent(struct snode *n, struct sbuf *b) {
    sb_printf(b, "MAJOR=8\nMINOR=%d\nDEVNAME=sd%c\nDEVTYPE=disk\n", n->id[0] * 16, 'a' + n->id[0]);
    return 0;
}

static const struct sent queue_children[] = {
    RO("hw_sector_size", show_sector_size),
    RO("logical_block_size", show_sector_size),
    RO("max_sectors_kb", show_max_sectors),
    RO("physical_block_size", show_sector_size),
    RO("rotational", show_zero),
    {0},
};
static const struct sent disk_children[] = {
    RO("dev", show_disk_dev),
    {.name = "queue", .type = VN_DIR, .mode = 0755, .children = queue_children},
    RO("removable", show_zero),
    RO("ro", show_zero),
    RO("size", show_disk_size),
    RO("uevent", show_uevent),
    {0},
};
static const struct sent disk_ent = {.name = "sd", .type = VN_DIR, .mode = 0755, .children = disk_children};

static int link_class_disk(struct snode *n, char *buf, size_t size) {
    return ksnprintf(buf, size, "../../block/sd%c", 'a' + n->id[0]);
}
static const struct sent class_disk_ent = {.name = "sd", .type = VN_LNK, .mode = 0777, .link = link_class_disk};

/* ---- the tree ---- */

static int show_thp(struct snode *n, struct sbuf *b) {
    (void)n;
    sb_printf(b, "always madvise [never]\n");
    return 0;
}

static int show_thp_defrag(struct snode *n, struct sbuf *b) {
    (void)n;
    sb_printf(b, "always defer defer+madvise madvise [never]\n");
    return 0;
}

static int show_hpage_size(struct snode *n, struct sbuf *b) {
    (void)n;
    sb_printf(b, "2097152\n");
    return 0;
}

static const struct sent thp_children[] = {
    RO("defrag", show_thp_defrag),
    RO("enabled", show_thp),
    RO("hpage_pmd_size", show_hpage_size),
    {0},
};
static const struct sent kernel_mm_children[] = {
    {.name = "transparent_hugepage", .type = VN_DIR, .mode = 0755, .children = thp_children},
    {0},
};
static const struct sent kernel_children[] = {
    {.name = "mm", .type = VN_DIR, .mode = 0755, .children = kernel_mm_children},
    {0},
};

static const struct sent system_children[] = {
    {.name = "cpu", .type = VN_DIR, .mode = 0755, .children = cpus_children, .dyn_ent = &cpu_ent, .dyn_slot = 0,
     .dyn_nth = cpu_nth, .dyn_find = cpu_find},
    {0},
};
static const struct sent devices_children[] = {
    {.name = "system", .type = VN_DIR, .mode = 0755, .children = system_children},
    {0},
};
static const struct sent empty_children[] = {{0}};
static const struct sent class_children[] = {
    {.name = "block", .type = VN_DIR, .mode = 0755, .dyn_ent = &class_disk_ent, .dyn_slot = 0,
     .dyn_nth = disk_nth, .dyn_find = disk_find},
    {.name = "net", .type = VN_DIR, .mode = 0755, .children = empty_children},
    {.name = "tty", .type = VN_DIR, .mode = 0755, .children = empty_children},
    {0},
};
static const struct sent root_children[] = {
    {.name = "block", .type = VN_DIR, .mode = 0755, .dyn_ent = &disk_ent, .dyn_slot = 0, .dyn_nth = disk_nth,
     .dyn_find = disk_find},
    {.name = "class", .type = VN_DIR, .mode = 0755, .children = class_children},
    {.name = "devices", .type = VN_DIR, .mode = 0755, .children = devices_children},
    {.name = "kernel", .type = VN_DIR, .mode = 0755, .children = kernel_children},
    {0},
};
static const struct sent sys_root = {.name = "sys", .type = VN_DIR, .mode = 0555, .children = root_children};

/* Another /sys (mount -t sysfs): the same tree. */
struct vnode *sysfs_root(void);
struct vnode *sysfs_root(void) { return synth_root(&sys_root); }

void sysfs_init(void) {
    struct vnode *root = synth_root(&sys_root);
    KASSERT(root);
    KASSERT(vfs_mount("/sys", root, "sysfs", MNT_NOSUID | MNT_NODEV | MNT_NOEXEC) == 0);
    vnode_put(root);
}
