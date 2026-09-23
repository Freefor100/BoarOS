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
int64_t kernel_futex_restart_timed(
    struct kernel_task *task, uint64_t address, uint32_t operation,
    uint32_t value, uint64_t deadline_ns,
    enum kernel_scheduler_status *status);
void kernel_futex_clear_tid(struct kernel_task *task);
void kernel_futex_release_robust(struct kernel_task *task, int32_t owner_tid);
void kernel_futex_set_robust_list(struct kernel_task *task, uint64_t head);
uint64_t kernel_futex_get_robust_list(const struct kernel_task *task);

#endif
