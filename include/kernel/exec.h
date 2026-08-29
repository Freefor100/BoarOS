#ifndef BOAROS_KERNEL_EXEC_H
#define BOAROS_KERNEL_EXEC_H

#include <stdint.h>

struct kernel_task;

enum kernel_exec_status {
    KERNEL_EXEC_STATUS_OK = 0,
    KERNEL_EXEC_STATUS_INVALID_ARGUMENT,
    KERNEL_EXEC_STATUS_NO_MEMORY,
    KERNEL_EXEC_STATUS_CLEANUP_REQUIRED,
    KERNEL_EXEC_STATUS_STATE,
};

/* A zero Linux result means a prepared image that must be committed. */
enum kernel_exec_status kernel_execve_prepare(
    struct kernel_task *task,
    uint64_t user_filename,
    uint64_t user_argv,
    uint64_t user_envp,
    int64_t *linux_result);

#endif
