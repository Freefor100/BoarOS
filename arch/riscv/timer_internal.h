#ifndef BOAROS_ARCH_RISCV_TIMER_INTERNAL_H
#define BOAROS_ARCH_RISCV_TIMER_INTERNAL_H

#include <arch/riscv/timer.h>

enum riscv_timer_status riscv_timer_deadline_advance(
    uint64_t now,
    uint64_t period,
    uint64_t previous_deadline,
    uint64_t *next_deadline,
    uint64_t *elapsed_ticks);

enum riscv_timer_status riscv_timer_event_advance(uint64_t now, uint64_t period,
    uint64_t tick_deadline, uint64_t scheduler_deadline,
    uint64_t *next_tick, uint64_t *elapsed_ticks);
#endif
