#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/lib.h>
#include <kernel/platform.h>
#include <kernel/proc.h>

/* Wall clock from the CMOS real-time clock at boot, then the TSC for
 * everything after (assumes an invariant TSC, true on every CPU we target). */

static uint64_t tsc_hz, tsc_boot;
static int64_t boot_epoch;

/* Measures the TSC over 50 ms of the (already calibrated) APIC tick. */
void time_init(void) {
    boot_epoch = platform_rtc_read();
    uint64_t start_tick = global_ticks;
    while (global_ticks == start_tick)
        cpu_idle_wait();
    uint64_t t0 = arch_counter(), tick0 = global_ticks;
    while (global_ticks < tick0 + HZ / 20)
        cpu_idle_wait();
    uint64_t t1 = arch_counter();
    tsc_hz = (t1 - t0) * 20;
    tsc_boot = t0 - (tick0 * tsc_hz / HZ);
    kprintf("time: TSC %lu MHz, wall clock %ld\n", tsc_hz / 1000000, boot_epoch);
}

uint64_t time_monotonic_ns(void) {
    if (!tsc_hz)
        return global_ticks * (1000000000ULL / HZ);
    uint64_t d = arch_counter() - tsc_boot;
    return d / tsc_hz * 1000000000ULL + (d % tsc_hz) * 1000000000ULL / tsc_hz;
}

int64_t time_realtime_s(void) { return boot_epoch + (int64_t)(time_monotonic_ns() / 1000000000ULL); }

/* Wall clock with the monotonic clock's resolution (boot_epoch + uptime). */
uint64_t time_realtime_ns(void) { return (uint64_t)boot_epoch * 1000000000ULL + time_monotonic_ns(); }
