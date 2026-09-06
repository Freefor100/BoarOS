#include <arch/riscv/timer.h>
#include <kernel/time.h>

#include <stdint.h>

#define KERNEL_TIME_NS_PER_SECOND UINT64_C(1000000000)
#define KERNEL_TIME_MULTIPLIER_SHIFT 32U

/*
 * Nanoseconds are derived from the arch time-counter through a fixed-point
 * multiplier `m = ceil(1e9 * 2^32 / frequency)` computed once at init, so
 * the hot path is one 64x64->128 multiply-high per read instead of a
 * division.  The rounding relative error stays below 2^-32; sleeps and
 * timeouts compensate by rounding deadlines in the tick domain.
 */
static uint32_t time_initialized;
static uint64_t time_multiplier;
static uint64_t time_boot_realtime_ns;

enum kernel_time_status kernel_time_init(uint32_t timebase_frequency,
                                         uint64_t boot_realtime_ns)
{
    if (timebase_frequency == 0U) {
        return KERNEL_TIME_STATUS_INVALID_ARGUMENT;
    }
    if (time_initialized != 0U) {
        return KERNEL_TIME_STATUS_ALREADY_INITIALIZED;
    }

    time_multiplier =
        ((KERNEL_TIME_NS_PER_SECOND << KERNEL_TIME_MULTIPLIER_SHIFT) +
         timebase_frequency - 1U) /
        timebase_frequency;
    time_boot_realtime_ns = boot_realtime_ns;
    time_initialized = 1U;
    return KERNEL_TIME_STATUS_OK;
}

uint32_t kernel_time_is_initialized(void)
{
    return time_initialized;
}

uint64_t kernel_time_ticks_to_ns(uint64_t ticks)
{
    return (uint64_t)(((unsigned __int128)ticks * time_multiplier) >>
                      KERNEL_TIME_MULTIPLIER_SHIFT);
}

uint64_t kernel_time_monotonic_ns(void)
{
    return kernel_time_ticks_to_ns(riscv_time_read());
}

uint64_t kernel_time_realtime_ns(void)
{
    return time_boot_realtime_ns + kernel_time_monotonic_ns();
}
