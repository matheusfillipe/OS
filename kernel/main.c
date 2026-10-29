#include <kernel/arch.h>
#include <kernel/blkdev.h>
#include <kernel/boot.h>
#include <kernel/drivers.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/net.h>
#include <kernel/pci.h>
#include <kernel/platform.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/vfs.h>

/* ---- Limine requests (placed in their own section, see linker.ld) ---- */

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(3);

__attribute__((used, section(".limine_requests")))
volatile struct limine_framebuffer_request boot_fb_request = {.id = LIMINE_FRAMEBUFFER_REQUEST_ID, .revision = 0};

__attribute__((used, section(".limine_requests")))
volatile struct limine_memmap_request boot_memmap_request = {.id = LIMINE_MEMMAP_REQUEST_ID, .revision = 0};

__attribute__((used, section(".limine_requests")))
volatile struct limine_hhdm_request boot_hhdm_request = {.id = LIMINE_HHDM_REQUEST_ID, .revision = 0};

__attribute__((used, section(".limine_requests")))
volatile struct limine_executable_address_request boot_kaddr_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID, .revision = 0};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_request = {.id = LIMINE_MODULE_REQUEST_ID, .revision = 0};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_cmdline_request cmdline_request = {.id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID,
                                                                             .revision = 0};

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end[] = LIMINE_REQUESTS_END_MARKER;

uint64_t hhdm_offset;

static struct cpu bsp_cpu;

/* ---- Boot self-tests: each prints a line the boot test looks for ---- */

static void selftest_memory(void) {
    arch_selftest_breakpoint();

    uint64_t before = pmm_free_pages();
    uint64_t a = pmm_alloc(1), b = pmm_alloc(8);
    KASSERT(a && b && a != b && (a % PAGE_SIZE) == 0);
    pmm_free(a, 1);
    pmm_free(b, 8);
    KASSERT(pmm_free_pages() == before);
    kprintf("selftest: pmm ok\n");

    extern char __text_start[];
    KASSERT(vmm_translate(vmm_kernel_pgd(), (uint64_t)__text_start) ==
            boot_kaddr_request.response->physical_base +
                ((uint64_t)__text_start - boot_kaddr_request.response->virtual_base));
    kprintf("selftest: vmm ok\n");

    void *ptrs[64];
    for (int i = 0; i < 64; i++) {
        ptrs[i] = kmalloc(16 + i * 40);
        KASSERT(ptrs[i] && ((uint64_t)ptrs[i] % 16) == 0);
        memset(ptrs[i], i, 16 + i * 40);
    }
    for (int i = 0; i < 64; i++)
        KASSERT(((uint8_t *)ptrs[i])[0] == i);
    for (int i = 0; i < 64; i += 2)
        kfree(ptrs[i]);
    for (int i = 1; i < 64; i += 2)
        kfree(ptrs[i]);
    void *big = kmalloc(1 << 20);
    KASSERT(big);
    kfree(big);
    kprintf("selftest: heap ok\n");
}

#define WORKERS 16
static volatile uint64_t cpus_used;
static volatile int workers_done;

static void worker(void *arg) {
    (void)arg;
    for (int i = 0; i < 20; i++) {
        __atomic_or_fetch(&cpus_used, 1ULL << (this_cpu()->id & 63), __ATOMIC_RELAXED);
        for (volatile int spin = 0; spin < 2000000; spin++)
            ;
        if (i % 2)
            thread_sleep_ms(4);
        else
            thread_yield();
    }
    __atomic_add_fetch(&workers_done, 1, __ATOMIC_RELEASE);
}

static volatile bool spinners_stop;
static volatile uint64_t spinner_counts[MAX_CPUS + 1];

/* Never yields: only preemption lets the others (and init) run. */
static void spinner(void *arg) {
    volatile uint64_t *count = arg;
    while (!__atomic_load_n(&spinners_stop, __ATOMIC_ACQUIRE))
        (*count)++;
}

static void wait_for(volatile int *counter, int target, const char *what) {
    uint64_t deadline = global_ticks + 10 * HZ;
    while (__atomic_load_n(counter, __ATOMIC_ACQUIRE) < target) {
        if (global_ticks > deadline)
            panic("selftest: timed out waiting for %s (%d/%d)", what, *counter, target);
        thread_sleep_ms(10);
    }
}

static void selftest_scheduler(void) {
    for (int i = 0; i < WORKERS; i++)
        KASSERT(thread_create("worker", worker, NULL));
    wait_for(&workers_done, WORKERS, "workers");
    int used = 0;
    for (uint64_t m = cpus_used; m; m &= m - 1)
        used++;
    if (cpu_count > 1 && used < 2)
        panic("selftest: %d workers all ran on one cpu", WORKERS);
    kprintf("selftest: scheduler ok (%d threads across %d cpus)\n", WORKERS, used);

    uint32_t n = cpu_count + 1; /* more spinners than cpus */
    for (uint32_t i = 0; i < n; i++)
        KASSERT(thread_create("spinner", spinner, (void *)&spinner_counts[i]));
    thread_sleep_ms(300);
    __atomic_store_n(&spinners_stop, true, __ATOMIC_RELEASE);
    for (uint32_t i = 0; i < n; i++)
        if (spinner_counts[i] == 0)
            panic("selftest: spinner %u never ran (no preemption?)", i);
    kprintf("selftest: preemption ok\n");
}


/* Modules whose string names an absolute path (limine.conf: module_string:
 * /usr/share/...) become read-only files there, backed by the memory the
 * bootloader loaded them into: a multi-gigabyte model costs no copy. */
static void files_from_modules(void) {
    struct limine_module_response *r = module_request.response;
    for (uint64_t i = 0; r && i < r->module_count; i++) {
        struct limine_file *m = r->modules[i];
        const char *dest = m->string;
        if (!dest || dest[0] != '/')
            continue;
        struct pageobj *o = pageobj_adopt(virt_to_phys_hhdm(m->address), m->size);
        if (!o || ramfs_add_object_file(dest, o, 0444) < 0) {
            kprintf("modules: could not place %s\n", dest);
            continue;
        }
        kprintf("modules: %s (%lu MiB, in place)\n", dest, (unsigned long)(m->size >> 20));
    }
}

static struct limine_file *find_module(const char *suffix) {
    struct limine_module_response *r = module_request.response;
    for (uint64_t i = 0; r && i < r->module_count; i++) {
        const char *path = r->modules[i]->path;
        size_t pl = strlen(path), sl = strlen(suffix);
        if (pl >= sl && !strcmp(path + pl - sl, suffix))
            return r->modules[i];
    }
    return NULL;
}

/* Development aid: QEMU can hand the OS a saved mind and design
 * (-fw_cfg name=opt/os/mind.json,file=... and opt/os/design.json) so
 * the desktop can be previewed without repeating the onboarding. */
void hda_init(void);

static void preload_state_from_fwcfg(void) {
    static const char *const names[] = {"mind.json", "design.json", "facet.xml", "app.env", "autorun.sh", "display"};
    static char buf[131072];
    for (size_t i = 0; i < ARRAY_LEN(names); i++) {
        char key[64], path[64];
        ksnprintf(key, sizeof key, "opt/os/%s", names[i]);
        int n = fwcfg_read_file(key, buf, sizeof buf);
        if (n <= 0)
            continue;
        vfs_mkdir_p("/etc/os", 0700);
        ksnprintf(path, sizeof path, "/etc/os/%s", names[i]);
        struct file *f;
        if (vfs_open(NULL, path, O_WRONLY | O_CREAT | O_TRUNC, 0600, &f) == 0) {
            file_write(f, buf, (size_t)n);
            file_put(f);
            kprintf("preloaded %s from the host\n", path);
        }
    }
}

/* Boot parameters: os.provider=, os.key=, os.model=, os.url= on
 * the kernel command line (limine.conf "cmdline:") become the AI config, so
 * a machine can start already connected. A config handed over by the VM
 * launcher (fw_cfg) wins. */
static bool param(const char *cmdline, const char *name, char *out, size_t n) {
    size_t l = strlen(name);
    for (const char *p = cmdline; p && *p;) {
        while (*p == 0x20)
            p++;
        if (!strncmp(p, name, l) && p[l] == 0x3d) {
            p += l + 1;
            size_t k = 0;
            while (*p && *p != 0x20 && k < n - 1)
                out[k++] = *p++;
            out[k] = 0;
            return true;
        }
        while (*p && *p != 0x20)
            p++;
    }
    return false;
}

static void json_str(char *dst, size_t n, const char *s) {
    size_t k = 0;
    for (; *s && k + 2 < n; s++)
        if (*s != 0x22 && *s != 0x5c && (unsigned char)*s >= 0x20)
            dst[k++] = *s;
    dst[k] = 0;
}

static void mind_from_cmdline(void) {
    struct limine_executable_cmdline_response *r = cmdline_request.response;
    if (!r || !r->cmdline || !r->cmdline[0])
        return;
    char provider[48] = "", key[256] = "", model[96] = "", url[160] = "";
    /* the old working name's prefix is still understood on boot menus written before the rename */
    const char *pre = param(r->cmdline, "os.provider", provider, sizeof provider) ? "os"
                      : param(r->cmdline, "lumen.provider", provider, sizeof provider) ? "lumen" : NULL;
    if (!pre)
        return;
    char name[24];
    ksnprintf(name, sizeof name, "%s.key", pre);
    param(r->cmdline, name, key, sizeof key);
    ksnprintf(name, sizeof name, "%s.model", pre);
    param(r->cmdline, name, model, sizeof model);
    ksnprintf(name, sizeof name, "%s.url", pre);
    param(r->cmdline, name, url, sizeof url);
    struct file *f;
    if (vfs_open(NULL, "/etc/os/mind.json", O_RDONLY, 0, &f) == 0) {
        file_put(f); /* already given one */
        return;
    }
    char p2[48], k2[256], m2[96], u2[160], buf[700];
    json_str(p2, sizeof p2, provider), json_str(k2, sizeof k2, key);
    json_str(m2, sizeof m2, model), json_str(u2, sizeof u2, url);
    int n = ksnprintf(buf, sizeof buf, "{\"provider\":\"%s\",\"base_url\":\"%s\",\"api_key\":\"%s\",\"model\":\"%s\"}\n",
                      p2, u2, k2, m2);
    vfs_mkdir_p("/etc/os", 0700);
    if (vfs_open(NULL, "/etc/os/mind.json", O_WRONLY | O_CREAT | O_TRUNC, 0600, &f) == 0) {
        file_write(f, buf, (size_t)n);
        file_put(f);
        kprintf("boot parameters: AI provider %s\n", p2);
    }
}

static void init_main(void *arg) {
    (void)arg;
    selftest_memory();
    selftest_scheduler();
    time_init();
    fpu_capture_clean_state();

    vfs_init();
    devfs_init();
    fbdev_init();
    input_init();
    struct limine_file *initrd = find_module("initrd.cpio");
    if (!initrd) {
        kprintf("no initrd module: kernel-only boot\n");
        kprintf("OS BOOT OK\n");
        platform_test_exit(0x00); /* QEMU exit code 1 = success in tests */
        return;
    }
    /* The base system first (the Freedesktop Platform and what is built on
     * it: /usr), then the OS's own files over it. */
    struct limine_file *base = find_module("base.cpio");
    if (base)
        initramfs_unpack(base->address, base->size);
    initramfs_unpack(initrd->address, initrd->size);
    /* Bundled applications (/apps), kept out of the core initrd. */
    struct limine_file *apps = find_module("apps.cpio");
    if (apps)
        initramfs_unpack(apps->address, apps->size);
    files_from_modules();
    preload_state_from_fwcfg();
    mind_from_cmdline();
    procfs_init(); /* after the initramfs: /proc and /sys are mounted over its directories */
    sysfs_init();
    pci_init();
    storage_init();
    hda_init(); /* drivers/hda.c: /dev/dsp, /dev/mixer */
    net_init();
    kprintf("OS BOOT OK\n");
    proc_start_init("/sbin/init");
}

void kmain(void);
void kmain(void) {
    platform_console_init();
    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision))
        panic("bootloader does not support Limine base revision 3");
    KASSERT(boot_hhdm_request.response);
    hhdm_offset = boot_hhdm_request.response->offset;

    fbcon_init();
    kprintf("Operating System kernel starting\n");

    arch_init_bsp(&bsp_cpu);
    cpus[0] = &bsp_cpu;

    pmm_init();
    kprintf("mm: %lu MiB usable, %lu MiB free\n", pmm_total_pages() * PAGE_SIZE >> 20,
            pmm_free_pages() * PAGE_SIZE >> 20);
    vmm_init();
    kprintf("mm: running on kernel page tables\n");
    heap_init();

    arch_init_interrupts(&bsp_cpu);
    sched_init_cpu();
    smp_init();

    KASSERT(thread_create("init", init_main, NULL));
    for (;;) /* this boot context is now CPU 0's idle thread */
        cpu_idle_wait();
}
