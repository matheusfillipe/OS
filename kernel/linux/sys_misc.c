#include <asm/elf.h>
#include <kernel/arch.h>
#include <kernel/blkdev.h>
#include <kernel/cpu.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/platform.h>
#include <kernel/proc.h>
#include <kernel/vfs.h>
#include <linux/syscall.h>

#define HOSTNAME_SIZE 65 /* the host name lives in the caller's UTS namespace (kernel/ns.h) */

SYSCALL(uname) {
    UNUSED_ARGS();
    struct linux_utsname u = {0};
    ksnprintf(u.sysname, sizeof u.sysname, "Linux"); /* programs check this; we speak its ABI */
    ksnprintf(u.nodename, sizeof u.nodename, "%s", uts_hostname());
    ksnprintf(u.release, sizeof u.release, "6.1.0-os");
    ksnprintf(u.version, sizeof u.version, "#1 SMP");
    ksnprintf(u.machine, sizeof u.machine, UTS_MACHINE);
    ksnprintf(u.domainname, sizeof u.domainname, "%s", uts_domainname());
    return copy_to_user(a1, &u, sizeof u);
}

SYSCALL(sethostname) {
    UNUSED_ARGS();
    if (a2 >= HOSTNAME_SIZE)
        return -EINVAL;
    char tmp[65] = {0};
    if (copy_from_user(tmp, a1, a2) < 0)
        return -EFAULT;
    memcpy(uts_hostname(), tmp, HOSTNAME_SIZE);
    return 0;
}

static void ns_to_timespec(uint64_t ns, struct linux_timespec *ts) {
    ts->tv_sec = (int64_t)(ns / 1000000000ULL);
    ts->tv_nsec = (int64_t)(ns % 1000000000ULL);
}


SYSCALL(clock_gettime) {
    UNUSED_ARGS();
    struct linux_timespec ts;
    switch (a1) {
    case CLOCK_REALTIME:
    case CLOCK_REALTIME_COARSE:
        ns_to_timespec(time_realtime_ns(), &ts);
        break;
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_MONOTONIC_COARSE:
    case CLOCK_BOOTTIME:
    case CLOCK_PROCESS_CPUTIME_ID:
    case CLOCK_THREAD_CPUTIME_ID:
        ns_to_timespec(time_monotonic_ns(), &ts);
        break;
    default:
        return -EINVAL;
    }
    return copy_to_user(a2, &ts, sizeof ts);
}

SYSCALL(clock_getres) {
    UNUSED_ARGS();
    if (!a2)
        return 0;
    struct linux_timespec ts = {0, 1};
    return copy_to_user(a2, &ts, sizeof ts);
}

SYSCALL(gettimeofday) {
    UNUSED_ARGS();
    if (a1) {
        uint64_t ns = time_realtime_ns();
        struct linux_timeval tv = {(int64_t)(ns / 1000000000ULL), (int64_t)(ns % 1000000000ULL / 1000)};
        if (copy_to_user(a1, &tv, sizeof tv) < 0)
            return -EFAULT;
    }
    return 0;
}

SYSCALL(time) {
    UNUSED_ARGS();
    int64_t t = time_realtime_s();
    if (a1 && copy_to_user(a1, &t, sizeof t) < 0)
        return -EFAULT;
    return t;
}

static int64_t sleep_until(uint64_t deadline_ns) {
    while (time_monotonic_ns() < deadline_ns) {
        if (signal_pending_current())
            return -EINTR;
        uint64_t left_ms = (deadline_ns - time_monotonic_ns()) / 1000000ULL;
        thread_sleep_ms(left_ms ? (left_ms < 50 ? left_ms : 50) : 1);
    }
    return 0;
}

SYSCALL(nanosleep) {
    UNUSED_ARGS();
    struct linux_timespec ts;
    if (copy_from_user(&ts, a1, sizeof ts) < 0)
        return -EFAULT;
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000)
        return -EINVAL;
    return sleep_until(time_monotonic_ns() + (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec);
}

SYSCALL(clock_nanosleep) {
    UNUSED_ARGS();
    struct linux_timespec ts;
    if (copy_from_user(&ts, a3, sizeof ts) < 0)
        return -EFAULT;
    uint64_t ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    if (a2 & 1) { /* TIMER_ABSTIME */
        uint64_t now = a1 == CLOCK_REALTIME ? time_realtime_ns() : time_monotonic_ns();
        return ns > now ? sleep_until(time_monotonic_ns() + (ns - now)) : 0;
    }
    return sleep_until(time_monotonic_ns() + ns);
}

SYSCALL(getrandom) {
    UNUSED_ARGS();
    uint8_t buf[256];
    uint64_t done = 0;
    while (done < a2) {
        uint64_t n = a2 - done < sizeof buf ? a2 - done : sizeof buf;
        random_bytes(buf, n);
        if (copy_to_user(a1 + done, buf, n) < 0)
            return done ? (int64_t)done : -EFAULT;
        done += n;
    }
    return (int64_t)done;
}

SYSCALL(sysinfo) {
    UNUSED_ARGS();
    struct linux_sysinfo si = {0};
    si.uptime = (int64_t)(time_monotonic_ns() / 1000000000ULL);
    si.totalram = pmm_total_pages() * PAGE_SIZE;
    si.freeram = pmm_free_pages() * PAGE_SIZE;
    si.procs = 1;
    si.mem_unit = 1;
    return copy_to_user(a1, &si, sizeof si);
}

SYSCALL(reboot) {
    UNUSED_ARGS();
    if (a1 != 0xfee1dead || (a2 != 672274793 && a2 != 85072278 && a2 != 369367448 && a2 != 537993216))
        return -EINVAL;
    kprintf("system %s requested by pid %d\n", a3 == LINUX_REBOOT_CMD_RESTART ? "restart" : "power-off",
            proc_current()->pid);
    /* Like Linux's device shutdown: drain the disks' write caches so data
     * written before power-off survives it. */
    void ext2_sync_all(void);
    ext2_sync_all(); /* and the disk file systems marked cleanly shut down */
    blkdev_sync_all();
    /* Under the QEMU test harness this ends the run; ACPI power-off proper
     * comes with the AML interpreter. Exit code: 1 = clean shutdown. */
    platform_test_exit(0x00);
    if (a3 == LINUX_REBOOT_CMD_RESTART)
        platform_reboot();
    platform_poweroff();
    halt_forever();
}

SYSCALL(syslog) {
    UNUSED_ARGS();
    return 0;
}

/* fadvise64: advice about how a file will be read; there is no page cache
 * to tune, so it is taken and changes nothing. */
SYSCALL(fadvise64) {
    UNUSED_ARGS();
    struct file *f = fd_get((int)a1);
    if (!f)
        return -EBADF;
    file_put(f);
    return 0;
}

/* personality: always plain Linux (PER_LINUX); asking (0xffffffff) says so. */
SYSCALL(personality) {
    UNUSED_ARGS();
    return 0;
}
