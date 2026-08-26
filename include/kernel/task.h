#ifndef BOAROS_KERNEL_TASK_H
#define BOAROS_KERNEL_TASK_H

#include <kernel/pid.h>

struct kernel_task;

enum kernel_task_status {
    KERNEL_TASK_STATUS_OK = 0,
    KERNEL_TASK_STATUS_INVALID_ARGUMENT,
    KERNEL_TASK_STATUS_STATE,
};

/* Returns the scheduler current task, or null before scheduler publication. */
struct kernel_task *kernel_task_current(void);

enum kernel_task_status kernel_task_tid(
    const struct kernel_task *task,
    kernel_pid_t *tid);

enum kernel_task_status kernel_task_tgid(
    const struct kernel_task *task,
    kernel_pid_t *tgid);

#endif
