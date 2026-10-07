#ifndef BOAROS_ARCH_TIMER_H
#define BOAROS_ARCH_TIMER_H
#if defined(BOAROS_ARCH_LOONGARCH)
#include <arch/loongarch/timer.h>
#else
#include <arch/riscv/timer.h>
#define arch_time_read riscv_time_read
#define arch_timer_status riscv_timer_status
#define arch_timer_set_scheduler_deadline riscv_timer_set_scheduler_deadline
#define ARCH_TIMER_STATUS_OK RISCV_TIMER_STATUS_OK
#define ARCH_TIMER_STATUS_NOT_STARTED RISCV_TIMER_STATUS_NOT_STARTED
#endif
#endif
