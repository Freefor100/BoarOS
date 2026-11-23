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
 * `boot_realtime_ns` points to the RTC sample captured at boot. A null
 * sample means there is no usable RTC and realtime starts at the boot epoch.
 * The hardware counter supplies the unadjusted clock and deadline domain.
 */
enum kernel_time_status kernel_time_init(uint32_t timebase_frequency,
                                        const uint64_t *boot_realtime_ns);

uint32_t kernel_time_is_initialized(void);
int kernel_time_has_rtc(void);

enum kernel_clock_id {
    KERNEL_CLOCK_REALTIME = 0,
    KERNEL_CLOCK_MONOTONIC = 1,
    KERNEL_CLOCK_BOOTTIME = 7,
    KERNEL_CLOCK_REALTIME_ALARM = 8,
    KERNEL_CLOCK_BOOTTIME_ALARM = 9,
    KERNEL_CLOCK_TAI = 11,
};

static inline int kernel_clock_is_alarm(int32_t clock)
{
    return clock == KERNEL_CLOCK_REALTIME_ALARM || clock == KERNEL_CLOCK_BOOTTIME_ALARM;
}

enum kernel_time_domain {
    KERNEL_TIME_MONOTONIC,
    KERNEL_TIME_REALTIME,
    KERNEL_TIME_TAI,
};

/* Relative ordinary realtime timers run in the monotonic domain. Alarms
 * convert the relative value to a deadline in their own clock, as does TAI. */
static inline enum kernel_time_domain kernel_time_timer_domain(int32_t clock, int absolute)
{
    if (clock == KERNEL_CLOCK_TAI) return KERNEL_TIME_TAI;
    if (clock == KERNEL_CLOCK_REALTIME_ALARM ||
        (clock == KERNEL_CLOCK_REALTIME && absolute)) return KERNEL_TIME_REALTIME;
    return KERNEL_TIME_MONOTONIC;
}

/* Continuous disciplined clock and unadjusted hardware clock, respectively. */
uint64_t kernel_time_monotonic_ns(void);
uint64_t kernel_time_raw_ns(void);

/* Keep a relative user timeout in the disciplined clock domain until park.
 * Linux ktime uses signed 64-bit nanoseconds. */
static inline uint64_t kernel_time_monotonic_after(uint64_t duration)
{
    uint64_t now=kernel_time_monotonic_ns();
    return now>=(uint64_t)INT64_MAX || duration>(uint64_t)INT64_MAX-now ?
        (uint64_t)INT64_MAX : now+duration;
}

/* Wall-clock nanoseconds: mutable offset plus monotonic, read consistently. */
uint64_t kernel_time_realtime_ns(void);
uint64_t kernel_time_tai_ns(void);

static inline uint64_t kernel_time_domain_now(enum kernel_time_domain domain)
{
    if (domain == KERNEL_TIME_TAI) return kernel_time_tai_ns();
    return domain == KERNEL_TIME_REALTIME ? kernel_time_realtime_ns() : kernel_time_monotonic_ns();
}

/* A timeout may be grouped with others only within its chosen slack.
 * Keep the original deadline for clock restart and user remaining time. */
uint64_t kernel_time_slack_deadline(enum kernel_time_domain, uint64_t deadline,
                                   uint64_t slack_ns);
struct kernel_task;
uint64_t kernel_time_select_deadline(const struct kernel_task *, uint64_t deadline);

/* Init and each delivered timer interrupt publish one actual counter sample.
 * Both coarse clocks derive from that same sample; reads do not touch CSR time. */
void kernel_time_update_coarse(void);
uint64_t kernel_time_coarse_monotonic_ns(void);
uint64_t kernel_time_coarse_realtime_ns(void);
uint64_t kernel_time_coarse_resolution_ns(void);

/* Converts time-counter ticks to nanoseconds (0 before init). */
uint64_t kernel_time_ticks_to_ns(uint64_t ticks);

/* Current REALTIME minus MONOTONIC offset; changed only by clock control. */
uint64_t kernel_time_boot_realtime_offset(void);

/*
 * Turns a monotonic-domain nanosecond timestamp into an absolute
 * time-counter deadline for the sleep path.  DEADLINE_PASSED means the
 * timestamp is not in the future and no sleep is needed.
 */
enum kernel_time_status kernel_time_deadline_from_monotonic(
    uint64_t target_monotonic_ns,
    uint64_t *deadline);

/* Scheduler execution budgets use unadjusted time. */
enum kernel_time_status kernel_time_deadline_from_raw(uint64_t ns,uint64_t *deadline);

/* Caller serializes clock control and holds the scheduler wait domain so that
 * publishing the offset and retargeting committed waits are one operation. */
enum kernel_time_status kernel_time_set_realtime(uint64_t nanoseconds);
void kernel_time_get_timezone(int32_t timezone[2]);
void kernel_time_set_timezone(const int32_t timezone[2]);
enum kernel_time_status kernel_time_deadline_from_realtime(uint64_t nanoseconds, uint64_t *deadline);
enum kernel_time_status kernel_time_deadline_from_tai(uint64_t nanoseconds, uint64_t *deadline);
#endif
