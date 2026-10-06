#ifndef BOAROS_ARCH_LOONGARCH_TIMER_H
#define BOAROS_ARCH_LOONGARCH_TIMER_H
#include <stdint.h>
enum arch_timer_status { ARCH_TIMER_STATUS_OK=0, ARCH_TIMER_STATUS_INVALID_ARGUMENT,
    ARCH_TIMER_STATUS_INVALID_FREQUENCY, ARCH_TIMER_STATUS_ALREADY_STARTED,
    ARCH_TIMER_STATUS_NOT_STARTED, ARCH_TIMER_STATUS_EARLY_INTERRUPT };
uint64_t arch_time_read(void);
uint32_t la_timer_frequency(void);
enum arch_timer_status la_timer_start(uint32_t, uint32_t);
enum arch_timer_status la_timer_interrupt(uint64_t *);
enum arch_timer_status arch_timer_set_scheduler_deadline(uint64_t);
#endif
