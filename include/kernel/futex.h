#ifndef BOAROS_KERNEL_FUTEX_H
#define BOAROS_KERNEL_FUTEX_H

#include <stdint.h>
#include <kernel/scheduler.h>

struct kernel_task;
struct kernel_mm;

int64_t kernel_futex(struct kernel_task *task, uint64_t address,
                     uint32_t operation, uint32_t value,
                     uint64_t timeout_or_count, uint64_t address2,
                     enum kernel_scheduler_status *status);
void kernel_futex_clear_tid(struct kernel_task *task);

#endif
