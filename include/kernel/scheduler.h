#ifndef BOAROS_KERNEL_SCHEDULER_H
#define BOAROS_KERNEL_SCHEDULER_H

#include <kernel/mm.h>
#include <kernel/physical_page.h>
#include <kernel/pid.h>

#include <stdint.h>

struct kernel_files;
struct kernel_fs_context;

enum kernel_scheduler_status {
    KERNEL_SCHEDULER_STATUS_OK = 0,
    KERNEL_SCHEDULER_STATUS_EMPTY,
    KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT,
    KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED,
    KERNEL_SCHEDULER_STATUS_ALREADY_INITIALIZED,
    KERNEL_SCHEDULER_STATUS_NO_MEMORY,
    KERNEL_SCHEDULER_STATUS_PAGE_ACCESS,
    KERNEL_SCHEDULER_STATUS_INVALID_STATE,
    KERNEL_SCHEDULER_STATUS_QUEUE_CORRUPT,
    KERNEL_SCHEDULER_STATUS_STACK_CORRUPT,
    KERNEL_SCHEDULER_STATUS_PAGE_RELEASE,
    KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE,
    KERNEL_SCHEDULER_STATUS_RESOURCE_CLEANUP,
};

enum kernel_thread_kind {
    KERNEL_THREAD_KIND_KERNEL = 0,
    KERNEL_THREAD_KIND_USER,
};

enum kernel_thread_exit_reason {
    KERNEL_THREAD_EXIT_RETURNED = 0,
    KERNEL_THREAD_EXIT_SYSCALL,
    KERNEL_THREAD_EXIT_USER_FAULT,
};

struct kernel_thread_completion {
    enum kernel_thread_kind kind;
    enum kernel_thread_exit_reason reason;
    kernel_pid_t tid;
    kernel_pid_t tgid;
    uint64_t status;
    uint64_t detail;
};

enum kernel_scheduler_status kernel_scheduler_init(
    struct physical_page_allocator *allocator,
    uintptr_t idle_stack_low,
    uintptr_t idle_stack_high);

enum kernel_scheduler_status kernel_thread_create(
    void (*entry)(void *),
    void *argument);

/*
 * Success consumes mm and, when non-null, the files/fs pair.  Failure
 * leaves every caller-owned resource unchanged.
 */
enum kernel_scheduler_status kernel_user_thread_create(
    struct kernel_mm *mm,
    struct kernel_files *files,
    struct kernel_fs_context *fs,
    uintptr_t entry,
    uintptr_t stack_pointer,
    uintptr_t thread_pointer);

enum kernel_scheduler_status kernel_scheduler_on_tick(
    uint64_t elapsed_ticks);

enum kernel_scheduler_status kernel_scheduler_reap_one(
    struct kernel_thread_completion *completion);

void kernel_thread_exit(void) __attribute__((noreturn));

void kernel_user_thread_exit(
    enum kernel_thread_exit_reason reason,
    uint64_t status,
    uint64_t detail) __attribute__((noreturn));

#endif
