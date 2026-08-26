#ifndef BOAROS_KERNEL_PID_H
#define BOAROS_KERNEL_PID_H

#include <stdint.h>

#define KERNEL_PID_BITMAP_WORDS(pid_count) \
    (((uint32_t)(pid_count) + 63U) / 64U)

typedef int32_t kernel_pid_t;

enum kernel_pid_status {
    KERNEL_PID_STATUS_OK = 0,
    KERNEL_PID_STATUS_INVALID,
    KERNEL_PID_STATUS_EXHAUSTED,
    KERNEL_PID_STATUS_NOT_ALLOCATED,
    KERNEL_PID_STATUS_STATE,
};

struct kernel_pid_allocator {
    uint64_t *bitmap;
    uint32_t limit;
    uint32_t cursor;
    uint32_t allocated;
    uint32_t initialized;
};

enum kernel_pid_status kernel_pid_allocator_init(
    struct kernel_pid_allocator *allocator,
    uint64_t *bitmap,
    uint32_t limit);

enum kernel_pid_status kernel_pid_allocate(
    struct kernel_pid_allocator *allocator,
    kernel_pid_t *pid);

enum kernel_pid_status kernel_pid_release(
    struct kernel_pid_allocator *allocator,
    kernel_pid_t pid);

#endif
