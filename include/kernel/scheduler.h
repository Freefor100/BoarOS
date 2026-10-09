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
    KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE,
    KERNEL_SCHEDULER_STATUS_RESOURCE_CLEANUP,
    KERNEL_SCHEDULER_STATUS_BUSY,
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

/* Measurements are accumulated only when a non-running stack is released.
 * No scan or allocation occurs on the tick/context-switch hot path. */
struct kernel_stack_statistics {
    uint64_t stacks_released;
    uint64_t minimum_free_bytes;
    uint64_t maximum_used_bytes;
};
void kernel_scheduler_stack_statistics(struct kernel_stack_statistics *statistics);
/* One registered kernel task owns potentially sleeping exit cleanup. */
void kernel_scheduler_register_cleanup(void);
void kernel_scheduler_wait_cleanup(uint64_t retry_deadline);
int kernel_scheduler_can_sleep(void);

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

void kernel_scheduler_rt_bandwidth_get(int64_t *period_us, int64_t *runtime_us);
int kernel_scheduler_rt_bandwidth_set(int runtime_field, int64_t value);

void kernel_scheduler_system_statistics(uint64_t loads[3], uint16_t *tasks);

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
struct kernel_wait_token {
    struct kernel_task *task;
    uint64_t generation;
};

typedef void (*kernel_wait_callback_fn)(struct kernel_wait_node *node,
                                        uint32_t reason);

/* 一个node最多登记一条queue；token绑定节点必须保持至wait_finish。
 * 队列借用不取得业务对象引用。回调在调度raw锁外同步执行，禁止抢占/阻塞；
 * 非阻塞remove只摘队，释放callback/context前须remove_sync收完在途借用。 */
struct kernel_wait_node {
    struct kernel_task *task;
    kernel_wait_callback_fn callback;
    void *context;
    struct kernel_wait_queue *queue;
    struct kernel_wait_node *previous;
    struct kernel_wait_node *next;
    struct kernel_wait_node *retired_next;
    struct kernel_wait_node *token_next;
    struct kernel_wait_queue *borrow_owner;
    uint64_t generation, sequence;
    uint32_t references;
};

/* 单CPU业务条件仍由调用者的IRQ纪律保护；短调度锁仅保护登记/通知。
 * close停止新登记，destroy在注册/游标/回调借用未归零时返回BUSY并保留owner。 */
struct kernel_wait_queue {
    uint32_t initialized;
    struct kernel_wait_node *head;
    struct kernel_wait_node *tail;
    uint64_t sequence;
    uint32_t closed, registrations, borrows;
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
enum kernel_scheduler_status kernel_wait_node_bind(struct kernel_wait_queue *,
    struct kernel_wait_node *, const struct kernel_wait_token *);
enum kernel_scheduler_status kernel_wait_node_remove_sync(struct kernel_wait_node *);
enum kernel_scheduler_status kernel_wait_queue_close(struct kernel_wait_queue *);
enum kernel_scheduler_status kernel_wait_queue_destroy(struct kernel_wait_queue *);
enum kernel_scheduler_status kernel_wait_prepare(struct kernel_wait_queue *,
    uint64_t deadline, int interruptible, struct kernel_wait_token *);
enum kernel_scheduler_status kernel_wait_park(const struct kernel_wait_token *,
    enum kernel_wait_wake_reason *);
enum kernel_scheduler_status kernel_wait_finish(struct kernel_wait_token *);
enum kernel_scheduler_status kernel_wait_notify(const struct kernel_wait_token *,
    enum kernel_wait_wake_reason);
void kernel_scheduler_switch_finish(void);
/* 无raw业务锁的单CPU调用边界：登记后只复查非阻塞的业务条件。
 * 需要释放对象raw的调用方使用显式prepare/park/finish，不能把锁放进表达式。 */
#define KERNEL_WAIT_RECHECK(queue_, deadline_, interruptible_, reason_, must_wait_) \
    ({ struct kernel_wait_token wait_recheck_token_; \
       enum kernel_wait_wake_reason *wait_recheck_reason_ = (reason_); \
       enum kernel_scheduler_status wait_recheck_status_ = wait_recheck_reason_ ? \
           kernel_wait_prepare((queue_), (deadline_), (interruptible_), &wait_recheck_token_) : \
           KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT; \
       if (wait_recheck_status_ == KERNEL_SCHEDULER_STATUS_OK) { \
           *wait_recheck_reason_ = KERNEL_WAIT_WOKEN; \
           if (must_wait_) wait_recheck_status_ = kernel_wait_park(&wait_recheck_token_, wait_recheck_reason_); \
           if (kernel_wait_finish(&wait_recheck_token_) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap(); \
       } \
       wait_recheck_status_; })

/* Wakes the longest-blocked waiter.  Requires interrupts disabled. */
enum kernel_scheduler_status kernel_wait_queue_wake_one(
    struct kernel_wait_queue *queue);
enum kernel_scheduler_status kernel_wait_queue_wake_all(
    struct kernel_wait_queue *queue);

/*
 * Sleeps until woken through `queue` (NULL = pure timeout sleep) or until
 * `deadline` time-counter ticks elapse (0 = no deadline).  Requires
 * interrupts disabled; the caller must recheck its condition on return.
 * 兼容包装为prepare→park→finish。持对象raw保护时须显式prepare并复查条件，
 * 释放raw后才能park；登记后的提前wake保留通知，不会再次提交阻塞。
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
/* The caller owns the handle until join has reclaimed the stopped stack. */
struct kernel_thread_join {
    struct kernel_task *task;
    struct kernel_wait_queue waiters;
};
enum kernel_scheduler_status kernel_thread_create_joinable(
    void (*entry)(void *), void *argument, struct kernel_thread_join *join);
void kernel_thread_join(struct kernel_thread_join *join);

enum kernel_scheduler_status kernel_scheduler_yield_current(void);

/*
 * Timer-interrupt path: credits `elapsed_ticks` to the interrupted task
 * as user or kernel time.  The idle task is never charged.  Call with
 * interrupts disabled before any switch decisions.
 */
void kernel_scheduler_charge_ticks(uint64_t elapsed_ticks, int from_user);
/* Tick-accounted time spent in the single-hart idle task. */
uint64_t kernel_scheduler_idle_ticks(void);
/* IRQ return may leave idle without waiting for a timer tick. */
void kernel_scheduler_prepare_idle_return(void);

/* Resolves a hardware fault in the running user task's active MM. */
enum kernel_mm_status kernel_scheduler_resolve_current_user_fault(
    uint64_t virtual_address,
    uint32_t access);

enum kernel_scheduler_status kernel_scheduler_reap_one(
    struct kernel_thread_completion *completion);

/* O(1) cleanup work predicate; call with interrupts disabled. */
int kernel_scheduler_reap_pending(void);

/* PID 1 shutdown, with interrupts disabled: terminate remaining user groups
 * through normal exit cleanup, leaving kernel I/O workers running.
 * Returns nonzero until their task/owner records have all been reaped. */
int kernel_scheduler_stop_users(void);

void kernel_thread_exit(void) __attribute__((noreturn));

void kernel_user_thread_exit(
    enum kernel_thread_exit_reason reason,
    uint64_t status,
    uint64_t detail) __attribute__((noreturn));

#endif
