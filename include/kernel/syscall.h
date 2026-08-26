#ifndef BOAROS_KERNEL_SYSCALL_H
#define BOAROS_KERNEL_SYSCALL_H

#include <stdint.h>

#define KERNEL_SYSCALL_ARGUMENT_COUNT 6U

struct kernel_task;

enum kernel_syscall_status {
    KERNEL_SYSCALL_STATUS_OK = 0,
    KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT,
};

enum kernel_syscall_action {
    KERNEL_SYSCALL_ACTION_RETURN = 0,
    KERNEL_SYSCALL_ACTION_EXIT,
};

struct kernel_syscall_request {
    uint64_t number;
    uint64_t arguments[KERNEL_SYSCALL_ARGUMENT_COUNT];
};

struct kernel_syscall_result {
    enum kernel_syscall_action action;
    int64_t value;
};

enum kernel_syscall_status kernel_syscall_dispatch(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *result);

#endif
