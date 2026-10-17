#include <arch/task.h>
#ifndef BOAROS_KERNEL_SCHED_PRIVATE_H
#define BOAROS_KERNEL_SCHED_PRIVATE_H

#include <kernel/cost.h>
#include <kernel/cpu.h>
#include <kernel/wait_internal.h>
#include <arch/context.h>
#include <kernel/files.h>
#include <kernel/futex.h>
#include <kernel/heap.h>
#include <kernel/fs_context.h>
#include <kernel/mm.h>
#include <kernel/physical_page.h>
#include <kernel/stack.h>
#include <kernel/pid.h>
#include <kernel/scheduler.h>
#include <kernel/sched_policy.h>
#include <kernel/sched_runqueue.h>
#include <kernel/signal.h>
#include <kernel/task.h>
#include <kernel/sync.h>

#include <stdint.h>

#define KERNEL_SCHEDULER_INITIALIZED UINT32_C(0x53434844)
#define KERNEL_THREAD_MAGIC UINT64_C(0x424f415254485244)
#define KERNEL_STACK_CANARY UINT64_C(0x535441434b4f4b21)
#define KERNEL_THREAD_NO_PAGE UINT64_MAX
#define KERNEL_STACK_FILL 0xa5U
#define KERNEL_PID_LIMIT 32768U

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
    /* 调度域保护phase；pending仅传递CPU0业务通知，不包含业务状态。 */
    uint32_t user_service, user_service_pending;
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
    uint64_t user_ticks;
    uint64_t kernel_ticks;
    uint64_t child_user_ticks;
    uint64_t child_kernel_ticks;
    uint64_t minor_faults;
    uint64_t major_faults;
    uint64_t child_minor_faults;
    uint64_t child_major_faults;
    uint64_t block_reads;
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
    /* 组身份拥有 alarm；仅排队的到期对象进入非持引用链。 */
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
} __attribute__((aligned(16)));

struct kernel_scheduler {
    uint32_t initialized;
    uint32_t shutdown_waiting;
    /* Last disposal may notify before cleanup registers: retain progress until
     * its next stop_users scan, without mistaking it for user quiescence. */
    uint32_t shutdown_progress;
    uint64_t kernel_context;
    struct kernel_cpu *cpus;
    struct kernel_task *all_tasks;
    struct kernel_task *retired_tasks;
    uint64_t retired_pending, task_count;
#if BOAROS_MAX_CPUS > 1
    uint32_t controlled_owners;
#endif
    uint64_t loads[3], load_ticks;
    struct kernel_pid_registry identities;
    struct kernel_heap identity_heap;
    struct physical_page_allocator *allocator;
    struct kernel_pid_allocator pid_allocator;
    uint64_t pid_bitmap[KERNEL_PID_BITMAP_WORDS(KERNEL_PID_LIMIT)];
    struct kernel_task boot_idle;
    struct kernel_task *cleanup_task;
    struct kernel_wait_queue cleanup_queue;
    struct kernel_sched_runqueue runqueue;
#if BOAROS_USER_SMP
    struct kernel_sched_runqueue rt_services;
#endif
    struct kernel_rt_configuration rt_configuration;
    struct kernel_task *exited_head;
    struct kernel_task *exited_tail;
    struct kernel_task *blocked_head;
    struct kernel_task *blocked_tail;
    struct kernel_task *deadline_root;
    struct kernel_task *stopped_head;
    struct kernel_task *stopped_tail;
    struct kernel_task *init_task;
    enum kernel_scheduler_status fatal_status;
    struct kernel_stack_statistics stack_statistics;
};

extern struct kernel_scheduler scheduler;
/* 已持调度域；变更资格不修改用户请求affinity，也不结束执行owner。 */
void scheduler_user_service_set_locked(struct kernel_task *, int);
void scheduler_user_request_locked(struct kernel_task *);
/* 同一调度域内结束逻辑实时资格；不结束任务执行或存储owner。 */
void scheduler_user_reservation_release_locked(struct kernel_task *);
static inline enum kernel_scheduler_status scheduler_failure(void)
{ return __atomic_load_n(&scheduler.fatal_status, __ATOMIC_RELAXED); }

/* Locked variants require kernel_wait_domain; a lookup borrow is obtained
 * under the same domain as discovery and lasts through task_put(). */
struct kernel_task *scheduler_task_lookup_locked(kernel_pid_t, enum kernel_pid_role, uint64_t);
struct kernel_task *scheduler_task_lookup(kernel_pid_t, enum kernel_pid_role, uint64_t);
void scheduler_task_get_locked(struct kernel_task *);
void scheduler_task_put(struct kernel_task *);
static inline void scheduler_task_borrow_end(struct kernel_task **task)
{ if (*task) scheduler_task_put(*task); }
#define SCHEDULER_TASK_BORROW(name, expression) \
    struct kernel_task *name __attribute__((cleanup(scheduler_task_borrow_end))) = (expression)
void scheduler_task_publish_locked(struct kernel_task *);
void scheduler_task_unlink_locked(struct kernel_task *);
void scheduler_task_retire(struct kernel_task *);
/* 已持调度域；资源清理已结束，跨解锁使用仍须存储borrow。 */
void scheduler_task_retire_locked(struct kernel_task *);
void scheduler_task_collect(void);
void scheduler_task_claim_release_locked(struct kernel_task *);
/* wait4 owns task storage through this call; release can expose an unconsumed
 * event or exited task, and notifies existing queue owners outside the domain. */
void scheduler_task_wait_claim_release(struct kernel_task *);
int scheduler_cleanup_ready_locked(void);
void exited_append_locked(struct kernel_task *);
void exited_remove_locked(struct kernel_task *);
enum kernel_scheduler_status scheduler_task_claim_exit_locked(struct kernel_task *);
enum kernel_scheduler_status scheduler_join_claim_locked(struct kernel_thread_join *, struct kernel_task **);
void scheduler_task_disposal_notify(void);
struct scheduler_group_cursor {
    struct kernel_task *leader, *task;
    uint64_t epoch;
    uint32_t member_owned, steps;
};
void scheduler_group_cursor_begin(struct kernel_task *, struct scheduler_group_cursor *);
void scheduler_group_cursor_next(struct scheduler_group_cursor *);
void scheduler_group_cursor_end(struct scheduler_group_cursor *);
struct scheduler_task_cursor { struct kernel_task *task; };
int scheduler_task_descends_from(struct kernel_task *, const struct kernel_task *, uint64_t);
void scheduler_task_cursor_begin(struct scheduler_task_cursor *);
void scheduler_task_cursor_next(struct scheduler_task_cursor *);
void scheduler_task_cursor_end(struct scheduler_task_cursor *);
void process_identity_commit_locked(struct kernel_task *);
void process_identity_sync_locked(struct kernel_task *);
void process_identity_release_locked(struct kernel_task *);

void kernel_proc_task_update_comm(struct kernel_task *task);

void scheduler_wake_task(struct kernel_task *thread, uint32_t reason);
enum kernel_pid_status process_identity_create(struct kernel_task *task,
    struct kernel_task *parent, int thread_clone);
void process_identity_release(struct kernel_task *task);
void process_identity_collect(void);
int process_group_is_orphaned(const struct kernel_task *task);
void process_orphan_notify(struct kernel_task *task, struct kernel_task *old_parent);
enum kernel_signal_status signal_send_kernel_group(struct kernel_task *task, uint32_t sig);
struct kernel_task *process_find_identity(kernel_pid_t number, enum kernel_pid_role role);
struct kernel_pid *process_identity(const struct kernel_task *task, enum kernel_pid_role role);
kernel_pid_t process_identity_number(const struct kernel_task *task, enum kernel_pid_role role);
uint64_t process_identity_generation(const struct kernel_task *task);
void process_group_initialize(struct kernel_task *task);
void process_group_request_exit(struct kernel_task *task,
                                enum kernel_thread_exit_reason reason,
                                uint64_t status, uint64_t detail);
enum kernel_scheduler_status process_group_exec_current(void);

/* 调用者独占从allocator取得的页对齐存储，完整清零后才发布对象。 */
void clear_page(void *pointer);
enum kernel_scheduler_status allocate_task_storage(struct kernel_task **task);
enum kernel_scheduler_status release_task_stack(struct kernel_task *task);
void kernel_task_release_io_scratch(struct kernel_task *task);
enum kernel_scheduler_status release_task_storage(struct kernel_task *task,
    enum kernel_scheduler_status original_status);
struct kernel_task *ready_first(void);
struct kernel_task *ready_next(const struct kernel_task *task);
void ready_remove(struct kernel_task *task);
void ready_enqueue(struct kernel_task *task, int head);
void scheduler_account_runtime(void);
void scheduler_account_runtime_locked(void);
struct kernel_task *scheduler_select_next_locked(struct kernel_cpu *cpu,
    struct kernel_task *previous, int ready_allowed);
enum kernel_scheduler_status scheduler_switch_claimed(
    struct kernel_task *previous, struct kernel_task *next);
void scheduler_cpu_initialize(struct kernel_cpu *cpu, struct kernel_task *idle,
                              uint64_t now_ns);
uint64_t scheduler_deadline_locked(struct kernel_cpu *cpu);
void scheduler_arm_timer(uint64_t deadline);
void scheduler_rearm_timer(void);
/* IRQ关闭；CPU/current借用只覆盖决策，架构切换后不得继续使用缓存值。 */
enum kernel_scheduler_status scheduler_reschedule(int rotate_other, int voluntary, int check_queues);
void ready_append(struct kernel_task *thread);
int scheduler_forget_task_locked(struct kernel_task *, struct kernel_wait_cursor *);
void scheduler_forget_task(struct kernel_task *thread);
void blocked_append(struct kernel_task *thread);
void blocked_unlink(struct kernel_task *thread);
void deadline_index_insert(struct kernel_task *task);
void deadline_index_remove(struct kernel_task *task);
struct kernel_task *deadline_index_first(void);
enum kernel_scheduler_status scheduler_switch_current_away(
    struct kernel_task *previous);
void process_complete_vfork(struct kernel_task *thread);
enum kernel_scheduler_status activate_thread_address_space(
    const struct kernel_task *thread);
enum kernel_scheduler_status validate_queues(void);
/* Caller holds the scheduler domain; checks and selection observe one state. */
enum kernel_scheduler_status validate_queues_locked(void);
enum kernel_scheduler_status release_after_create_failure(
    uint64_t physical_address,
    enum kernel_scheduler_status original_status);
enum kernel_scheduler_status validate_current(void);
/* CPU obtained at this IRQ-off entry; never retain it across switch/sleep. */
enum kernel_scheduler_status validate_current_on_cpu(struct kernel_cpu *cpu);
enum kernel_task_status validate_task_resource_borrow(
    const struct kernel_task *task);
void wake_waiting_parent(struct kernel_task *child);
void stopped_append_locked(struct kernel_task *);
void stopped_unlink_locked(struct kernel_task *);
void stopped_append(struct kernel_task *thread);
void stopped_unlink(struct kernel_task *thread);
enum kernel_signal_status kernel_signal_release_table(
    struct kernel_task *task);


void ready_enqueue_locked(struct kernel_task *task, int head);
struct kernel_task *ready_best_locked(void);
void ready_remove_locked(struct kernel_task *task);
#endif
