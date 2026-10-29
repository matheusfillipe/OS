#include <kernel/arch.h>
#include <kernel/lib.h>
#include <kernel/platform.h>
#include <kernel/proc.h>

/* Wall clock from the real-time clock at boot, then the arch cycle counter for
 * everything after (it must tick at a constant rate on every CPU). */

static uint64_t counter_hz, counter_boot;
static int64_t boot_epoch;

/* Measures the cycle counter over 50 ms of the (already calibrated) timer tick. */
void time_init(void) {
    boot_epoch = platform_rtc_read();
    uint64_t start_tick = global_ticks;
    while (global_ticks == start_tick)
        cpu_idle_wait();
    uint64_t t0 = arch_counter(), tick0 = global_ticks;
    while (global_ticks < tick0 + HZ / 20)
        cpu_idle_wait();
    uint64_t t1 = arch_counter();
    counter_hz = (t1 - t0) * 20;
    counter_boot = t0 - (tick0 * counter_hz / HZ);
    kprintf("time: counter %lu MHz, wall clock %ld\n", counter_hz / 1000000, boot_epoch);
}

uint64_t time_monotonic_ns(void) {
    if (!counter_hz)
        return global_ticks * (1000000000ULL / HZ);
    uint64_t d = arch_counter() - counter_boot;
    return d / counter_hz * 1000000000ULL + (d % counter_hz) * 1000000000ULL / counter_hz;
}

int64_t time_realtime_s(void) { return boot_epoch + (int64_t)(time_monotonic_ns() / 1000000000ULL); }

/* Wall clock with the monotonic clock's resolution (boot_epoch + uptime). */
uint64_t time_realtime_ns(void) { return (uint64_t)boot_epoch * 1000000000ULL + time_monotonic_ns(); }
