#ifndef BOAROS_KERNEL_CPU_H
#define BOAROS_KERNEL_CPU_H
#include <arch/context.h>
#include <arch/switch_context.h>
#include <kernel/sched_policy.h>
#include <kernel/cpu_config.h>
#include <kernel/cost.h>
#if BOAROS_MAX_CPUS > 1
#include <kernel/sched_runqueue.h>
#endif
#include <stdint.h>

#define KERNEL_CPU_INITIALIZED UINT32_C(0x43505531)
struct kernel_task;
struct kernel_io_context;
struct kernel_raw_guard;
#if BOAROS_MAX_CPUS > 1
struct kernel_cpu_notification_batch {
    struct kernel_cpu *cpu;
    uintptr_t interrupts;
    uint32_t previous_depth, targets;
};
enum kernel_cpu_state {
    KERNEL_CPU_PREPARED = 1, KERNEL_CPU_STARTING, KERNEL_CPU_ONLINE,
    KERNEL_CPU_QUIESCING, KERNEL_CPU_STOPPED
};
#endif
/* CPU本地借用须处于IRQ关闭或禁止抢占范围；任务I/O owner不随CPU保存。 */
struct kernel_cpu {
    uint64_t hardware_id;
    struct kernel_task *current;
    /* 定位校验与本地raw状态靠近current；远程请求保留在记录尾部。 */
    struct kernel_raw_guard *raw_locks;
    uint32_t initialized, preempt_depth;
#if BOAROS_COST_DIAGNOSTICS
    struct kernel_cost_cpu cost;
#endif
    struct kernel_task *switch_previous;
    /* 由实际栈交接转移；current可提前发布下一任务。 */
    struct kernel_task *accounting_owner;
    uint64_t accounting_started_ticks;
    uint64_t accounting_user_ticks, accounting_kernel_ticks;
#if BOAROS_MAX_CPUS > 1
    /* 本CPU在IRQ关闭下写；远端读者用代次重试，owner仍由调度域转移。 */
    uint64_t accounting_sequence;
#endif
    /* 当前地址空间记录借用由执行task的MM owner覆盖，切换失效后才清旧bit。 */
    void *active_mm_record;
    uint64_t active_mm_generation;
    uint64_t active_mm_instruction_generation;
    struct kernel_task *idle;
    struct kernel_cpu *scheduler_next;
    struct kernel_rt_bandwidth rt_bandwidth;
    struct arch_switch_context discard_context;
    uint64_t idle_ticks, timer_deadline;
    unsigned idle_context_saved;
    struct kernel_io_context *bootstrap_io;
    unsigned need_resched;
    unsigned rotate_other;
    uint64_t wait_notification_sequence;
    unsigned wait_notification_depth;
#if BOAROS_MAX_CPUS > 1
    uint32_t logical_id, state, events, outbound;
    struct { uint64_t period, next_deadline; int armed; } timer;
    struct kernel_sched_runqueue eligible;
    uint64_t ipi_sent, ipi_received, rfence_requests;
#if BOAROS_USER_SMP
    /* 借用任务执行/就绪owner，保留服务往返期间的实时用户资格。 */
    struct kernel_sched_runqueue user_reservations;
    uint64_t user_traps, user_returns, user_local_interrupts;
    uint64_t syscall_dispatches;
#if BOAROS_COST_DIAGNOSTICS
    uint64_t service_migrations, service_queue_ticks;
    uint64_t service_samples, service_elapsed_ticks, service_running_ns;
#endif
#endif
    /* 栈上通知批次只在本CPU禁止抢占、IRQ关闭期间借用。 */
    struct kernel_cpu_notification_batch *notification_batch;
#endif
};
#if BOAROS_COST_DIAGNOSTICS
_Static_assert(offsetof(struct kernel_cpu, cost) == 32, "cost assembly CPU prefix");
#endif
#if BOAROS_MAX_CPUS > 1
static inline int kernel_cpu_online(const struct kernel_cpu *cpu)
{ return __atomic_load_n(&cpu->state, __ATOMIC_ACQUIRE) == KERNEL_CPU_ONLINE; }
#endif
static inline struct kernel_cpu *kernel_cpu_current(void)
{
    struct kernel_cpu *cpu = arch_current_cpu_get();
    if (!cpu || cpu->initialized != KERNEL_CPU_INITIALIZED) __builtin_trap();
    return cpu;
}
/* Validated boot topology, published before mounts and unchanged thereafter.
 * This counts configured executors, including secondaries not yet started. */
void kernel_cpu_set_boot_count(unsigned count);
unsigned kernel_cpu_get_boot_count(void);
void kernel_cpu_initialize(struct kernel_cpu *, uint64_t, struct kernel_io_context *);
void kernel_cpu_boot_initialize(uint64_t hardware_id);
void kernel_cpu_boot_rebind(void);
void kernel_preempt_disable(void);
void kernel_preempt_enable(void);
void kernel_assert_can_block(void);
#if BOAROS_MAX_CPUS > 1
/* raw外发送；操作批次先归还对象资格，再提交其中的远端目标。 */
void kernel_cpu_flush_notifications(void);
void kernel_cpu_notify(struct kernel_cpu *, unsigned events);
/* 对象资格取得后begin，全部归还后end；批次内不允许阻塞。 */
void kernel_cpu_notification_batch_begin(struct kernel_cpu_notification_batch *);
void kernel_cpu_notification_batch_end(struct kernel_cpu_notification_batch *);
struct kernel_task *kernel_cpu_secondary_prepare(struct kernel_cpu *, uint64_t, unsigned);
void kernel_cpu_secondary_attach(struct kernel_cpu *, uintptr_t stack_low, uintptr_t stack_high);
void kernel_assert_service_context(void);
#else
struct kernel_cpu_notification_batch {};
static inline void kernel_cpu_notification_batch_begin(struct kernel_cpu_notification_batch *batch)
{ (void)batch; }
static inline void kernel_cpu_notification_batch_end(struct kernel_cpu_notification_batch *batch)
{ (void)batch; }
static inline void kernel_assert_service_context(void) {}
#endif
static inline void kernel_cpu_request_schedule(struct kernel_cpu *cpu)
{
    __atomic_store_n(&cpu->need_resched, 1U, __ATOMIC_RELEASE);
#if BOAROS_MAX_CPUS > 1
    if (cpu != kernel_cpu_current()) kernel_cpu_notify(cpu, 1U);
#endif
}
static inline unsigned kernel_cpu_schedule_requested(struct kernel_cpu *cpu)
{ return __atomic_load_n(&cpu->need_resched, __ATOMIC_ACQUIRE); }
static inline unsigned kernel_cpu_consume_schedule(struct kernel_cpu *cpu)
{ return __atomic_exchange_n(&cpu->need_resched, 0U, __ATOMIC_ACQ_REL); }
#endif
