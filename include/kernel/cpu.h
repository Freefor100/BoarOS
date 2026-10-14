#ifndef BOAROS_KERNEL_CPU_H
#define BOAROS_KERNEL_CPU_H
#include <arch/context.h>
#include <arch/switch_context.h>
#include <kernel/sched_policy.h>
#include <kernel/cpu_config.h>
#if BOAROS_MAX_CPUS > 1
#include <kernel/sched_runqueue.h>
#endif
#include <stdint.h>

#define KERNEL_CPU_INITIALIZED UINT32_C(0x43505531)
struct kernel_task;
struct kernel_io_context;
struct kernel_raw_guard;
#if BOAROS_MAX_CPUS > 1
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
    struct kernel_task *switch_previous;
    /* 当前地址空间记录借用由执行task的MM owner覆盖，切换失效后才清旧bit。 */
    void *active_mm_record;
    uint64_t active_mm_generation;
    struct kernel_task *idle;
    struct kernel_cpu *scheduler_next;
    struct kernel_rt_bandwidth rt_bandwidth;
    struct arch_switch_context discard_context;
    uint64_t idle_ticks, armed_deadline, timer_deadline;
    unsigned idle_context_saved;
    struct kernel_io_context *bootstrap_io;
    unsigned need_resched;
    unsigned rotate_other;
#if BOAROS_MAX_CPUS > 1
    uint32_t logical_id, state, events, outbound;
    struct { uint64_t period, next_deadline; int armed; } timer;
    struct kernel_sched_runqueue eligible;
    uint64_t ipi_sent, ipi_received, rfence_requests;
#if BOAROS_USER_SMP
    uint64_t user_traps, user_returns, user_local_interrupts;
    uint64_t service_dispatches, service_migrations, service_queue_ticks;
    uint64_t service_samples, service_elapsed_ticks, service_running_ns;
#endif
#endif
};
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
void kernel_cpu_initialize(struct kernel_cpu *, uint64_t, struct kernel_io_context *);
void kernel_cpu_boot_initialize(uint64_t hardware_id);
void kernel_cpu_boot_rebind(void);
void kernel_preempt_disable(void);
void kernel_preempt_enable(void);
void kernel_assert_can_block(void);
#if BOAROS_MAX_CPUS > 1
/* 固件通知仅在本CPU最后一个raw释放之后发送。 */
void kernel_cpu_flush_notifications(void);
void kernel_cpu_notify(struct kernel_cpu *, unsigned events);
struct kernel_task *kernel_cpu_secondary_prepare(struct kernel_cpu *, uint64_t, unsigned);
void kernel_cpu_secondary_attach(struct kernel_cpu *, uintptr_t stack_low, uintptr_t stack_high);
void kernel_assert_service_context(void);
#else
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
