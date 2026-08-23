#ifndef BOAROS_KERNEL_SCHEDULER_H
#define BOAROS_KERNEL_SCHEDULER_H

#include <kernel/physical_page.h>

#include <stdint.h>

enum kernel_scheduler_status {
    KERNEL_SCHEDULER_STATUS_OK = 0,
    KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT,
    KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED,
    KERNEL_SCHEDULER_STATUS_ALREADY_INITIALIZED,
    KERNEL_SCHEDULER_STATUS_NO_MEMORY,
    KERNEL_SCHEDULER_STATUS_PAGE_ACCESS,
    KERNEL_SCHEDULER_STATUS_INVALID_STATE,
    KERNEL_SCHEDULER_STATUS_QUEUE_CORRUPT,
    KERNEL_SCHEDULER_STATUS_STACK_CORRUPT,
    KERNEL_SCHEDULER_STATUS_PAGE_RELEASE,
};

enum kernel_scheduler_status kernel_scheduler_init(
    struct physical_page_allocator *allocator,
    uintptr_t idle_stack_low,
    uintptr_t idle_stack_high);

enum kernel_scheduler_status kernel_thread_create(
    void (*entry)(void *),
    void *argument);

enum kernel_scheduler_status kernel_scheduler_on_tick(
    uint64_t elapsed_ticks);

enum kernel_scheduler_status kernel_scheduler_reap_exited(
    uint64_t *reaped_count);

void kernel_thread_exit(void) __attribute__((noreturn));

#endif
