#ifndef BOAROS_KERNEL_SCHEDULER_H
#define BOAROS_KERNEL_SCHEDULER_H

#include <kernel/mm.h>
#include <kernel/physical_page.h>
#include <kernel/pid.h>

#include <stdint.h>

struct kernel_files;
struct kernel_fs_context;
struct kernel_task;

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
    KERNEL_THREAD_EXIT_RESOURCE,
    KERNEL_THREAD_EXIT_SIGNAL,
};

enum kernel_thread_resource {
    KERNEL_THREAD_RESOURCE_NO_MEMORY = 1,
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

/* Commits the image prepared by the current user task's exec transaction. */
enum kernel_scheduler_status kernel_scheduler_exec_commit(void);
void kernel_user_group_exit(enum kernel_thread_exit_reason reason,
                            uint64_t status, uint64_t detail)
    __attribute__((noreturn));

/*
 * Linux riscv64 asm-generic rusage layout (144 bytes): only ru_utime and
 * ru_stime carry real values; the remaining fields are zero until the
 * kernel tracks those resources.
 */
struct kernel_linux_rusage {
    struct {
        int64_t tv_sec;
        int64_t tv_usec;
    } ru_utime;
    struct {
        int64_t tv_sec;
        int64_t tv_usec;
    } ru_stime;
    int64_t ru_maxrss;
    int64_t ru_ixrss;
    int64_t ru_idrss;
    int64_t ru_isrss;
    int64_t ru_minflt;
    int64_t ru_majflt;
    int64_t ru_nswap;
    int64_t ru_inblock;
    int64_t ru_oublock;
    int64_t ru_msgsnd;
    int64_t ru_msgrcv;
    int64_t ru_nsignals;
    int64_t ru_nvcsw;
    int64_t ru_nivcsw;
};

enum kernel_scheduler_status kernel_scheduler_wait4_current(
    int64_t pid,
    uint64_t status_address,
    uint32_t options,
    uint64_t rusage_address,
    int64_t *linux_result);

enum kernel_wait_wake_reason {
    KERNEL_WAIT_WOKEN = 0,
    KERNEL_WAIT_TIMEOUT = 1,
    KERNEL_WAIT_SIGNALLED = 2,
};

#define KERNEL_WAIT_QUEUE_INITIALIZED UINT32_C(0x57414954)

struct kernel_wait_node;

typedef void (*kernel_wait_callback_fn)(struct kernel_wait_node *node,
                                        uint32_t reason);

struct kernel_wait_node {
    struct kernel_task *task;
    kernel_wait_callback_fn callback;
    void *context;
    struct kernel_wait_queue *queue;
    struct kernel_wait_node *previous;
    struct kernel_wait_node *next;
};

/*
 * Token identifying a sleep channel.  Resources that can block embed one
 * queue per wake condition and hand it to block/wake.
 */
struct kernel_wait_queue {
    uint32_t initialized;
    struct kernel_wait_node *head;
    struct kernel_wait_node *tail;
};

void kernel_wait_node_init(struct kernel_wait_node *node,
                           struct kernel_task *task);
void kernel_wait_node_init_callback(struct kernel_wait_node *node,
                                    kernel_wait_callback_fn callback,
                                    void *context);
void kernel_wait_queue_init(struct kernel_wait_queue *queue);
void kernel_wait_queue_add(struct kernel_wait_queue *queue,
                           struct kernel_wait_node *node);
void kernel_wait_queue_remove(struct kernel_wait_node *node);

/* Wakes the longest-blocked waiter.  Requires interrupts disabled. */
enum kernel_scheduler_status kernel_wait_queue_wake_one(
    struct kernel_wait_queue *queue);
enum kernel_scheduler_status kernel_wait_queue_wake_all(
    struct kernel_wait_queue *queue);

/*
 * Sleeps until woken through `queue` (NULL = pure timeout sleep) or until
 * `deadline` time-counter ticks elapse (0 = no deadline).  Requires
 * interrupts disabled; the caller must recheck its condition on return.
 */
enum kernel_scheduler_status kernel_scheduler_block_current(
    struct kernel_wait_queue *queue,
    uint64_t deadline,
    int interruptible,
    enum kernel_wait_wake_reason *wake_reason);

/* Wakes an interruptible waiter so a pending signal can be delivered. */
enum kernel_scheduler_status kernel_scheduler_wake_signal(
    struct kernel_task *task);

/*
 * Timer-interrupt path: wakes every blocked task whose deadline has
 * passed.  Requires interrupts disabled; `now` is the time-counter value.
 */
enum kernel_scheduler_status kernel_scheduler_expire_deadlines(uint64_t now);

/* Requeues the current task behind any ready task and switches away. */
enum kernel_scheduler_status kernel_scheduler_yield_current(void);

/*
 * Timer-interrupt path: credits `elapsed_ticks` to the interrupted task
 * as user or kernel time.  The idle task is never charged.  Call with
 * interrupts disabled before any switch decisions.
 */
void kernel_scheduler_charge_ticks(uint64_t elapsed_ticks, int from_user);

/* Resolves a hardware fault in the running user task's active MM. */
enum kernel_mm_status kernel_scheduler_resolve_current_user_fault(
    uint64_t virtual_address,
    uint32_t access);

enum kernel_scheduler_status kernel_scheduler_reap_one(
    struct kernel_thread_completion *completion);

/* O(1) cleanup work predicate; call with interrupts disabled. */
int kernel_scheduler_reap_pending(void);

void kernel_thread_exit(void) __attribute__((noreturn));

void kernel_user_thread_exit(
    enum kernel_thread_exit_reason reason,
    uint64_t status,
    uint64_t detail) __attribute__((noreturn));

#endif
