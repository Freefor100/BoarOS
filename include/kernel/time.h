#ifndef BOAROS_KERNEL_TIME_H
#define BOAROS_KERNEL_TIME_H

#include <stdint.h>

enum kernel_time_status {
    KERNEL_TIME_STATUS_OK = 0,
    KERNEL_TIME_STATUS_INVALID_ARGUMENT,
    KERNEL_TIME_STATUS_ALREADY_INITIALIZED,
    KERNEL_TIME_STATUS_NOT_INITIALIZED,
    KERNEL_TIME_STATUS_DEADLINE_PASSED,
};

/*
 * `boot_realtime_ns` is the wall-clock reading captured at boot (0 when no
 * RTC source is available; CLOCK_REALTIME then tracks time since boot).
 * Monotonic and deadline values use the arch time-counter directly.
 */
enum kernel_time_status kernel_time_init(uint32_t timebase_frequency,
                                         uint64_t boot_realtime_ns);

uint32_t kernel_time_is_initialized(void);

/* Nanoseconds since boot, derived from the arch time-counter. */
uint64_t kernel_time_monotonic_ns(void);

/* Wall-clock nanoseconds: boot reading plus monotonic. */
uint64_t kernel_time_realtime_ns(void);

/* Converts time-counter ticks to nanoseconds (0 before init). */
uint64_t kernel_time_ticks_to_ns(uint64_t ticks);

/* The wall-clock reading captured at boot (0 without an RTC source). */
uint64_t kernel_time_boot_realtime_offset(void);

/*
 * Turns a monotonic-domain nanosecond timestamp into an absolute
 * time-counter deadline for the sleep path.  DEADLINE_PASSED means the
 * timestamp is not in the future and no sleep is needed.
 */
enum kernel_time_status kernel_time_deadline_from_monotonic(
    uint64_t target_monotonic_ns,
    uint64_t *deadline);

#endif
