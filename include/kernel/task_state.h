#ifndef BOAROS_KERNEL_TASK_STATE_H
#define BOAROS_KERNEL_TASK_STATE_H

/* Internal canonical task storage. Public interfaces keep kernel_task opaque;
 * code using an executing task's embedded state shares this exact layout. */
#include <arch/task.h>
#include <kernel/cost.h>
#include <kernel/files.h>
#include <kernel/fs_context.h>
#include <kernel/futex.h>
#include <kernel/mm.h>
#include <kernel/pid.h>
#include <kernel/sched_policy.h>
#include <kernel/sched_runqueue.h>
#include <kernel/scheduler.h>
#include <kernel/signal.h>
#include <kernel/sync.h>
#include <kernel/task.h>
#include <kernel/wait_internal.h>
#include <stdint.h>

struct kernel_task_usage {
    uint64_t user_ns, kernel_ns;
    uint64_t minor_faults, major_faults, maxrss_pages;
    uint64_t voluntary, involuntary, read_bytes, write_bytes;
};

struct kernel_exec_transaction;
struct kernel_memory_object;

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
    KERNEL_SYSCALL_RESTART_FUTEX_TIMED,
};

struct kernel_syscall_restart_state {
    uint32_t kind;
    uint32_t reserved;
    union {
        struct {
            uint64_t deadline;
            uint64_t remaining_address;
        } nanosleep;
        struct {
            uint64_t address;
            uint64_t deadline_ns;
            uint32_t operation;
            uint32_t expected;
            uint32_t bitset;
        } futex_timed;
    } value;
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

enum kernel_user_return_work {
    KERNEL_USER_RETURN_CPU0 = 1U,
    KERNEL_USER_RETURN_SIGNAL = 2U,
};

struct kernel_task {
    struct arch_thread_state arch;
    struct kernel_cpu *cpu;
    uint64_t magic;
    uint64_t physical_address;
    uint64_t stack_physical_address;
    uintptr_t stack_low;
    uintptr_t stack_high;
    struct kernel_task *next;
    struct kernel_task *all_next;
    struct kernel_task *all_previous;
    struct kernel_task *retired_next;
    struct kernel_task *exit_previous;
    uint32_t exit_queued, reaping;
    /* Domain-protected storage borrows; they do not pin MM/files/business state.
     * Retirement 1 stops discovery, 2 finishes resources, 3 owns disposal. */
    uint32_t storage_borrows, retirement;
    struct kernel_pid *prepared_group, *prepared_session;
    struct kernel_sched_policy scheduling;
    struct kernel_sched_node ready_node;
#if BOAROS_MAX_CPUS > 1
    struct kernel_sched_node eligible_nodes[BOAROS_MAX_CPUS];
    /* controlled=1为私有准备，2持已发布存储owner，直到最终页归还。 */
    uint32_t cpu_mask, controlled;
    uint64_t bound_mm_context;
#endif
#if BOAROS_USER_SMP
    /* 调度域保护phase；返回通知区分CPU0业务和执行者本地信号，不复制业务载荷。 */
    uint32_t user_service, user_return_pending;
    struct kernel_sched_node service_node, reservation_node;
    struct kernel_cpu *reservation_cpu;
    uint32_t user_slice_expired;
#if BOAROS_COST_DIAGNOSTICS
    uint64_t service_started_ticks, service_running_ns;
#endif
#endif
    unsigned accounted;
    struct kernel_thread_join *join;
    struct kernel_task *parent;
    uint64_t children_epoch, group_epoch;
    struct kernel_task *first_child;
    struct kernel_task *last_child;
    struct kernel_task *previous_sibling;
    struct kernel_task *next_sibling;
    uint32_t state;
    uint32_t idle;
    kernel_pid_t tid;
    struct kernel_pid_member identities[KERNEL_PID_ROLES];
    uint32_t tid_owned;
    uint32_t publish_completion;
    uint32_t wait_status;
    uint64_t set_tid_address;
    uint64_t clear_tid_address;
    uint64_t robust_list_head;
    struct kernel_wait_record wait;
    /* Intrusive ordered index over tasks with a deadline, keyed by
     * (wait.deadline, tid); maintained with interrupts disabled. */
    struct kernel_task *deadline_left;
    struct kernel_task *deadline_right;
    int8_t deadline_height;
    uint8_t deadline_indexed;
    struct kernel_syscall_restart_state syscall_restart;
    struct kernel_socket_read_request *socket_read_request;
    struct kernel_socket_write_request *socket_write_request;
    struct kernel_epoll_wait_request *epoll_wait_request;
    struct kernel_task_io_buffer *io_buffer;
    /* Task-owned scratch page retained across sequential I/O calls. */
    struct physical_page_allocator *io_scratch_allocator;
    uint64_t io_scratch_physical_address;
    void *io_scratch_data;
    struct kernel_tty_request *tty_request;
    struct kernel_tty *controlling_tty;
    struct kernel_io_context io_context;
    uint32_t accounting_mode;
    struct kernel_task_usage usage, dead_usage, child_usage;
    uint64_t proc_start_ticks;
    char comm[16];
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
    /* 组身份拥有定时器；活动索引在最后成员清理前摘除。 */
    struct kernel_task *real_timer_next;
    uint64_t real_timer_deadline;
    uint64_t real_timer_interval;
    uint32_t real_timer_active;
    struct kernel_pid *child_creator;
    uint32_t terminate_requested;
    uint32_t proc_exiting;
    uint32_t group_exiting;
    uint32_t group_execing;
    uint32_t fork_no_exec;
    uint32_t session_leader;
    uint32_t group_stopped;
    struct kernel_rlimit64 nofile_limit;
    struct kernel_rlimit64 stack_limit;
    struct kernel_wait_queue group_wait_queue;
    uint64_t group_pending;
    uint32_t group_sender[KERNEL_SIGNAL_COUNT];
    int16_t group_signal_code[KERNEL_SIGNAL_COUNT];
    struct kernel_signal_child_info group_child_info;
    struct kernel_wait_node default_wait_node;
    struct kernel_task *blocked_previous;
    struct kernel_futex_key futex_key;
    uint32_t futex_bitset;
    struct kernel_signal_fault signal_fault;
    uint64_t signal_pending;
    uint64_t signal_blocked;
    uint64_t signal_wait_mask;
    uint64_t signal_saved_mask;
    uint32_t signal_restore_mask;
    uint64_t signal_table_address;
    uint32_t signal_sender[KERNEL_SIGNAL_COUNT];
    int16_t signal_code[KERNEL_SIGNAL_COUNT];
    struct kernel_signal_child_info signal_child_info;
    uint32_t stop_notified;
    uint32_t continue_notified;
    struct kernel_thread_completion completion;
    struct kernel_files files;
    struct kernel_fs_context fs;
    struct arch_fpu_state fpu;
    struct kernel_mm mm;
    struct kernel_exec_transaction *exec_transaction;
    struct arch_switch_context context;
#if BOAROS_COST_DIAGNOSTICS
    struct kernel_cost_task cost;
#endif
    /* 稀用的进程CPU定时器状态置尾，保持普通信号返回路径的原有字段布局。 */
    struct kernel_task *cpu_timer_next;
    struct { uint64_t expires, interval; } cpu_timers[2];
    uint32_t cpu_timer_active;
    struct kernel_rlimit64 core_limit;
    struct kernel_rlimit64 fsize_limit;
} __attribute__((aligned(16)));

#endif
