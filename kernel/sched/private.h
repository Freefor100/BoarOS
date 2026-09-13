#ifndef BOAROS_KERNEL_SCHED_PRIVATE_H
#define BOAROS_KERNEL_SCHED_PRIVATE_H

#include <arch/riscv/context.h>
#include <arch/riscv/fpu.h>
#include <arch/riscv/thread.h>
#include <kernel/files.h>
#include <kernel/fs_context.h>
#include <kernel/mm.h>
#include <kernel/physical_page.h>
#include <kernel/pid.h>
#include <kernel/scheduler.h>
#include <kernel/signal.h>
#include <kernel/task.h>

#include <stdint.h>

#define KERNEL_SCHEDULER_INITIALIZED UINT32_C(0x53434844)
#define KERNEL_THREAD_MAGIC UINT64_C(0x424f415254485244)
#define KERNEL_STACK_CANARY UINT64_C(0x535441434b4f4b21)
#define KERNEL_THREAD_NO_PAGE UINT64_MAX
#define KERNEL_THREAD_MINIMUM_STACK 512U
#define KERNEL_PID_LIMIT 32768U

struct kernel_exec_transaction;

enum kernel_thread_state {
    KERNEL_THREAD_STATE_IDLE = 0,
    KERNEL_THREAD_STATE_READY,
    KERNEL_THREAD_STATE_RUNNING,
    KERNEL_THREAD_STATE_BLOCKED,
    KERNEL_THREAD_STATE_EXITED,
    KERNEL_THREAD_STATE_ZOMBIE,
    KERNEL_THREAD_STATE_STOPPED,
    KERNEL_THREAD_STATE_GROUP_DEAD,
};

enum kernel_syscall_restart_kind {
    KERNEL_SYSCALL_RESTART_NONE = 0,
    KERNEL_SYSCALL_RESTART_GENERIC,
    KERNEL_SYSCALL_RESTART_NANOSLEEP,
};

/* Signal state per task.  `signal_pending`/`signal_blocked` are bitmaps
 * with bit (sig - 1); `signal_table_address` is a lazily allocated page
 * of dispositions (0 until the first rt_sigaction).  The sender array
 * keeps the first sender of each pending standard signal. */
#define KERNEL_SIGNAL_TABLE_MAGIC UINT64_C(0x5349475441424C45)

struct kernel_signal_action {
    uint64_t handler;
    uint64_t flags;
    uint64_t mask;
};

struct kernel_signal_table {
    uint64_t magic;
    uint32_t references;
    struct kernel_signal_action actions[KERNEL_SIGNAL_COUNT];
};

struct kernel_task {
    struct riscv_thread_state arch;
    uint64_t magic;
    uint64_t physical_address;
    uintptr_t stack_low;
    uintptr_t stack_high;
    struct kernel_task *next;
    struct kernel_task *parent;
    struct kernel_task *first_child;
    struct kernel_task *last_child;
    struct kernel_task *previous_sibling;
    struct kernel_task *next_sibling;
    uint32_t state;
    uint32_t idle;
    kernel_pid_t tid;
    kernel_pid_t process_group;
    uint32_t tid_owned;
    uint32_t publish_completion;
    uint32_t wait_status;
    uint64_t clear_tid_address;
    struct kernel_wait_queue *wait_queue;
    uint64_t wakeup_deadline;
    uint32_t wake_reason;
    uint32_t wait_interruptible;
    uint32_t syscall_restart_kind;
    uint32_t syscall_restart_reserved;
    uint64_t syscall_restart_deadline;
    uint64_t syscall_restart_remaining_address;
    uint64_t user_ticks;
    uint64_t kernel_ticks;
    uint64_t child_user_ticks;
    uint64_t child_kernel_ticks;
    struct kernel_wait_queue child_exit_queue;
    struct kernel_wait_queue vfork_done_queue;
    uint32_t vfork_child;
    uint32_t vfork_waiting;
    struct kernel_task *vfork_parent;
    struct kernel_task *vfork_wait_child;
    struct kernel_task *group_leader;
    uint32_t group_members;
    struct kernel_task *group_next;
    struct kernel_task *group_previous;
    kernel_pid_t child_creator_tid;
    uint32_t terminate_requested;
    uint32_t group_exiting;
    uint32_t group_execing;
    uint32_t group_stopped;
    struct kernel_wait_queue group_wait_queue;
    uint64_t group_pending;
    uint32_t group_sender[KERNEL_SIGNAL_COUNT];
    struct kernel_task *wait_previous;
    struct kernel_task *wait_next;
    struct kernel_task *blocked_previous;
    uint64_t futex_mm;
    uint64_t futex_address;
    uint64_t signal_pending;
    uint64_t signal_blocked;
    uint64_t signal_saved_mask;
    uint32_t signal_restore_mask;
    uint64_t signal_table_address;
    uint32_t signal_sender[KERNEL_SIGNAL_COUNT];
    uint32_t stop_notified;
    uint32_t continue_notified;
    struct kernel_thread_completion completion;
    struct kernel_files files;
    struct kernel_fs_context fs;
    struct riscv_fpu_state fpu;
    struct kernel_mm mm;
    struct kernel_exec_transaction *exec_transaction;
    struct riscv_switch_context context;
} __attribute__((aligned(16)));

struct kernel_scheduler {
    uint32_t initialized;
    uint32_t idle_context_saved;
    uint64_t kernel_satp;
    struct physical_page_allocator *allocator;
    struct kernel_pid_allocator pid_allocator;
    uint64_t pid_bitmap[KERNEL_PID_BITMAP_WORDS(KERNEL_PID_LIMIT)];
    struct kernel_task idle;
    struct kernel_task *current;
    struct kernel_task *ready_head;
    struct kernel_task *ready_tail;
    struct kernel_task *exited_head;
    struct kernel_task *exited_tail;
    struct kernel_task *blocked_head;
    struct kernel_task *blocked_tail;
    struct kernel_task *stopped_head;
    struct kernel_task *stopped_tail;
    struct kernel_task *init_task;
    uint64_t cleanup_page_address;
    uint32_t cleanup_page_owned;
    enum kernel_scheduler_status fatal_status;
    struct riscv_switch_context discard_context;
};

extern struct kernel_scheduler scheduler;

void scheduler_wake_task(struct kernel_task *thread, uint32_t reason);
void scheduler_wait_requeue(struct kernel_task *task,
                            struct kernel_wait_queue *queue);
void process_group_initialize(struct kernel_task *task);
void process_group_request_exit(struct kernel_task *task,
                                enum kernel_thread_exit_reason reason,
                                uint64_t status, uint64_t detail);
enum kernel_scheduler_status process_group_exec_current(void);
int kernel_signal_has_pending(const struct kernel_task *task);

uintptr_t align_up_16(uintptr_t value);
void clear_page(void *pointer);
void ready_append(struct kernel_task *thread);
struct kernel_task *ready_pop(void);
void blocked_append(struct kernel_task *thread);
void blocked_unlink(struct kernel_task *thread);
enum kernel_scheduler_status scheduler_switch_current_away(
    struct kernel_task *previous);
void process_complete_vfork(struct kernel_task *thread);
enum kernel_scheduler_status activate_thread_address_space(
    const struct kernel_task *thread);
enum kernel_scheduler_status validate_queues(void);
enum kernel_scheduler_status release_after_create_failure(
    uint64_t physical_address,
    enum kernel_scheduler_status original_status);
enum kernel_scheduler_status validate_current(void);
enum kernel_task_status validate_task_resource_borrow(
    const struct kernel_task *task);
void wake_waiting_parent(struct kernel_task *child);
void stopped_append(struct kernel_task *thread);
void stopped_unlink(struct kernel_task *thread);
enum kernel_signal_status kernel_signal_release_table(
    struct kernel_task *task);

#endif
