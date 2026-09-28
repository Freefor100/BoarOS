#ifndef BOAROS_KERNEL_TASK_H
#define BOAROS_KERNEL_TASK_H

#include <kernel/pid.h>
#include <kernel/rlimit.h>

struct kernel_task;
struct kernel_mm;
struct kernel_files;
struct kernel_fs_context;
struct kernel_socket_read_request;

struct physical_page_allocator;
/* One scratch page owned by a live I/O invocation, including forced exit. */
struct kernel_task_io_buffer {
    struct kernel_task *task;
    struct physical_page_allocator *allocator;
    uint64_t physical_address;
    void *data;
};

enum kernel_task_status {
    KERNEL_TASK_STATUS_OK = 0,
    KERNEL_TASK_STATUS_INVALID_ARGUMENT,
    KERNEL_TASK_STATUS_RESOURCE_UNAVAILABLE,
    KERNEL_TASK_STATUS_STATE,
};

/* Returns the scheduler current task, or null before scheduler publication. */
struct kernel_task *kernel_task_current(void);
enum kernel_task_status kernel_task_socket_read_register(
    struct kernel_task *task, struct kernel_socket_read_request *request);
enum kernel_task_status kernel_task_socket_read_clear(
    struct kernel_task *task, struct kernel_socket_read_request *request);

enum kernel_task_status kernel_task_io_buffer_acquire(
    struct kernel_task_io_buffer *buffer, struct physical_page_allocator *allocator);
void kernel_task_io_buffer_release(struct kernel_task_io_buffer *buffer);

void kernel_task_prepare_user_return(void);

/* Records the set_tid_address clear pointer and returns the caller tid. */
enum kernel_task_status kernel_task_set_tid_address(
    struct kernel_task *task,
    uint64_t address,
    kernel_pid_t *tid);

enum kernel_task_status kernel_task_tid(
    const struct kernel_task *task,
    kernel_pid_t *tid);

enum kernel_task_status kernel_task_tgid(
    const struct kernel_task *task,
    kernel_pid_t *tgid);

enum kernel_task_status kernel_task_get_rlimit(
    const struct kernel_task *task, uint32_t resource,
    struct kernel_rlimit64 *limit);
enum kernel_task_status kernel_task_set_rlimit(
    struct kernel_task *task, uint32_t resource,
    const struct kernel_rlimit64 *limit);

/* Current user task's stack soft limit; default capacity for kernel setup. */
uint64_t kernel_task_current_stack_limit(void);

enum kernel_task_status kernel_task_ppid(
    const struct kernel_task *task,
    kernel_pid_t *ppid);

/* The returned MM is borrowed for the duration of the current task call. */
enum kernel_task_status kernel_task_mm_borrow(
    const struct kernel_task *task,
    const struct kernel_mm **mm);

/* The current task may mutate its MM through the returned borrowed handle. */
enum kernel_task_status kernel_task_mm_borrow_mutable(
    struct kernel_task *task,
    struct kernel_mm **mm);

/* The returned file table is borrowed and may be mutated by this syscall. */
enum kernel_task_status kernel_task_files_borrow(
    struct kernel_task *task,
    struct kernel_files **files);

enum kernel_task_status kernel_task_fs_context_borrow(
    const struct kernel_task *task,
    const struct kernel_fs_context **fs);

/*
 * CPU accounting in scheduler ticks: own user/kernel time plus the rolled
 * up time of reaped children.
 */
void kernel_task_cpu_ticks(const struct kernel_task *task,
                           uint64_t *user_ticks,
                           uint64_t *kernel_ticks,
                           uint64_t *child_user_ticks,
                           uint64_t *child_kernel_ticks);

#endif
