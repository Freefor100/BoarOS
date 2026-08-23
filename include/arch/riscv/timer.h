#ifndef BOAROS_ARCH_RISCV_TIMER_H
#define BOAROS_ARCH_RISCV_TIMER_H

#include <stdint.h>

enum riscv_timer_status {
    RISCV_TIMER_STATUS_OK = 0,
    RISCV_TIMER_STATUS_INVALID_ARGUMENT,
    RISCV_TIMER_STATUS_INVALID_FREQUENCY,
    RISCV_TIMER_STATUS_ALREADY_STARTED,
    RISCV_TIMER_STATUS_SBI_PROBE_FAILED,
    RISCV_TIMER_STATUS_SBI_TIME_UNAVAILABLE,
    RISCV_TIMER_STATUS_SBI_SET_FAILED,
    RISCV_TIMER_STATUS_NOT_STARTED,
    RISCV_TIMER_STATUS_EARLY_INTERRUPT,
};

enum riscv_timer_status riscv_timer_start(uint32_t timebase_frequency,
                                          uint32_t ticks_per_second);
enum riscv_timer_status riscv_timer_handle_interrupt(
    uint64_t *elapsed_ticks);

#endif
