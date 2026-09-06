#include <arch/riscv/sbi.h>
#include <arch/riscv/timer.h>

#include "timer_internal.h"

#include <stddef.h>
#include <stdint.h>

#define SBI_EXT_TIME 0x54494d45UL
#define RISCV_SIE_STIE (1UL << 5)

struct riscv_timer_state {
    uint64_t period;
    uint64_t next_deadline;
    int armed;
};

static struct riscv_timer_state timer_state;

uint64_t riscv_time_read(void)
{
    uint64_t value;

    asm volatile("csrr %0, time" : "=r"(value));
    return value;
}

enum riscv_timer_status riscv_timer_deadline_advance(
    uint64_t now,
    uint64_t period,
    uint64_t previous_deadline,
    uint64_t *next_deadline,
    uint64_t *elapsed_ticks)
{
    uint64_t delta;
    uint64_t computed_elapsed;
    uint64_t computed_next;

    if (period == 0U || next_deadline == NULL || elapsed_ticks == NULL) {
        return RISCV_TIMER_STATUS_INVALID_ARGUMENT;
    }

    delta = now - previous_deadline;
    if ((delta >> 63) != 0U) {
        return RISCV_TIMER_STATUS_EARLY_INTERRUPT;
    }

    computed_elapsed = delta / period + 1U;
    computed_next = previous_deadline + computed_elapsed * period;
    *next_deadline = computed_next;
    *elapsed_ticks = computed_elapsed;
    return RISCV_TIMER_STATUS_OK;
}

enum riscv_timer_status riscv_timer_start(uint32_t timebase_frequency,
                                          uint32_t ticks_per_second)
{
    uint64_t period;
    uint64_t deadline;
    long probe_result;
    unsigned long stie = RISCV_SIE_STIE;

    if (timebase_frequency == 0U || ticks_per_second == 0U ||
        timebase_frequency < ticks_per_second) {
        return RISCV_TIMER_STATUS_INVALID_FREQUENCY;
    }
    if (timer_state.armed) {
        return RISCV_TIMER_STATUS_ALREADY_STARTED;
    }

    probe_result = sbi_probe_extension(SBI_EXT_TIME);
    if (probe_result < 0L) {
        return RISCV_TIMER_STATUS_SBI_PROBE_FAILED;
    }
    if (probe_result == 0L) {
        return RISCV_TIMER_STATUS_SBI_TIME_UNAVAILABLE;
    }

    period = timebase_frequency / ticks_per_second;
    deadline = riscv_time_read() + period;
    if (sbi_set_timer(deadline) != 0L) {
        return RISCV_TIMER_STATUS_SBI_SET_FAILED;
    }

    timer_state.period = period;
    timer_state.next_deadline = deadline;
    timer_state.armed = 1;
    asm volatile("csrs sie, %0\n"
                 "csrsi sstatus, 2"
                 :
                 : "r"(stie)
                 : "memory");
    return RISCV_TIMER_STATUS_OK;
}

enum riscv_timer_status riscv_timer_handle_interrupt(
    uint64_t *elapsed_ticks)
{
    uint64_t candidate_deadline;
    uint64_t candidate_elapsed;
    enum riscv_timer_status status;

    if (elapsed_ticks == NULL) {
        return RISCV_TIMER_STATUS_INVALID_ARGUMENT;
    }
    if (!timer_state.armed) {
        return RISCV_TIMER_STATUS_NOT_STARTED;
    }

    status = riscv_timer_deadline_advance(riscv_time_read(),
                                          timer_state.period,
                                          timer_state.next_deadline,
                                          &candidate_deadline,
                                          &candidate_elapsed);
    if (status != RISCV_TIMER_STATUS_OK) {
        return status;
    }
    if (sbi_set_timer(candidate_deadline) != 0L) {
        return RISCV_TIMER_STATUS_SBI_SET_FAILED;
    }

    timer_state.next_deadline = candidate_deadline;
    *elapsed_ticks = candidate_elapsed;
    return RISCV_TIMER_STATUS_OK;
}
