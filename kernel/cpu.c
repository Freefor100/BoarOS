#include <kernel/cpu.h>
#include <kernel/raw_lock.h>

#if BOAROS_MAX_CPUS > 1
static unsigned boot_count;
#endif

#if BOAROS_USER_SMP
#include <kernel/wait_internal.h>

static struct run_lock_slot { struct kernel_raw_lock lock; } __attribute__((aligned(64)))
    run_locks[BOAROS_MAX_CPUS] = { [0 ... BOAROS_MAX_CPUS-1] = {
        .lock={.rank=KERNEL_RAW_RANK_RUNQUEUE,.initialized=KERNEL_RAW_INITIALIZED} } };

/* 只由持共同控制锁的慢路径使用；guard存储不会跨越切栈。 */
static struct kernel_raw_guard run_guards[BOAROS_MAX_CPUS];

struct kernel_raw_lock *kernel_run_lock(const struct kernel_cpu *cpu)
{ return &run_locks[cpu->logical_id].lock; }

void kernel_run_set_acquire(struct kernel_run_set *set)
{
    if (!kernel_raw_lock_owned(&kernel_wait_domain)) __builtin_trap();
    if (run_guards[0].lock) return;
    unsigned count=__atomic_load_n(&boot_count,__ATOMIC_ACQUIRE);
    /* Before topology publication the bootstrap/model may still use all slots. */
    if (!count) count=BOAROS_MAX_CPUS;
    for (unsigned i=0;i<count;i++)
        kernel_raw_lock_acquire(&run_locks[i].lock,&run_guards[i]);
    set->held=count;
}

void kernel_run_set_release(struct kernel_run_set *set)
{
    if (!set->held) return;
    for (unsigned i=set->held;i;)
        kernel_raw_lock_release(&run_guards[--i]);
    set->held=0;
}
#endif

void kernel_cpu_set_boot_count(unsigned count)
{
    if (!count || count > BOAROS_MAX_CPUS) __builtin_trap();
#if BOAROS_MAX_CPUS > 1
    unsigned unpublished=0;
    if (!__atomic_compare_exchange_n(&boot_count,&unpublished,count,0,
                                    __ATOMIC_RELEASE,__ATOMIC_RELAXED)) __builtin_trap();
#endif
}
unsigned kernel_cpu_get_boot_count(void)
{
#if BOAROS_MAX_CPUS > 1
    unsigned count=__atomic_load_n(&boot_count,__ATOMIC_ACQUIRE);
    if (!count) __builtin_trap();
    return count;
#else
    return 1;
#endif
}

void kernel_cpu_initialize(struct kernel_cpu *cpu, uint64_t id,
                           struct kernel_io_context *bootstrap_io)
{
    if (!cpu) __builtin_trap();
    *cpu = (struct kernel_cpu){ .hardware_id = id, .bootstrap_io = bootstrap_io,
                               .initialized = KERNEL_CPU_INITIALIZED };
}
void kernel_preempt_disable(void)
{
    uintptr_t irq = arch_interrupt_save();
    struct kernel_cpu *cpu = kernel_cpu_current();
    if (cpu->preempt_depth == UINT32_MAX) __builtin_trap();
    cpu->preempt_depth++;
    arch_interrupt_restore(irq);
}
void kernel_preempt_enable(void)
{
    uintptr_t irq = arch_interrupt_save();
    struct kernel_cpu *cpu = kernel_cpu_current();
    if (!cpu->preempt_depth || (cpu->raw_locks &&
        (cpu->raw_locks->previous_depth == UINT32_MAX ||
         cpu->preempt_depth <= cpu->raw_locks->previous_depth + 1U))) __builtin_trap();
    cpu->preempt_depth--;
    /* 不在任意调用栈发起切换；待调度请求由既有安全返回/tick点消费。 */
    arch_interrupt_restore(irq);
}
void kernel_assert_can_block(void)
{
    uintptr_t irq = arch_interrupt_save();
    struct kernel_cpu *cpu = kernel_cpu_current();
    if (cpu->raw_locks || cpu->preempt_depth) __builtin_trap();
    arch_interrupt_restore(irq);
}
#if BOAROS_MAX_CPUS > 1
void kernel_cpu_notification_batch_begin(struct kernel_cpu_notification_batch *batch)
{
    uintptr_t irq = arch_interrupt_save();
    struct kernel_cpu *cpu = kernel_cpu_current();
    if (!batch || cpu->raw_locks || cpu->notification_batch ||
        cpu->preempt_depth == UINT32_MAX) __builtin_trap();
    *batch = (struct kernel_cpu_notification_batch){
        .cpu = cpu, .interrupts = irq, .previous_depth = cpu->preempt_depth,
    };
    cpu->preempt_depth++;
    cpu->notification_batch = batch;
}
void kernel_cpu_notification_batch_end(struct kernel_cpu_notification_batch *batch)
{
    struct kernel_cpu *cpu = kernel_cpu_current();
    if (!batch || batch->cpu != cpu || cpu->notification_batch != batch ||
        cpu->raw_locks || arch_interrupt_is_enabled() ||
        batch->previous_depth == UINT32_MAX ||
        cpu->preempt_depth != batch->previous_depth + 1U) __builtin_trap();
    /* 先结束栈借用；调用者归还对象资格后，才执行实际的固件通知。 */
    cpu->notification_batch = 0;
    cpu->outbound |= batch->targets;
    if (cpu->outbound) kernel_cpu_flush_notifications();
    cpu->preempt_depth = batch->previous_depth;
    uintptr_t irq = batch->interrupts;
    *batch = (struct kernel_cpu_notification_batch){0};
    arch_interrupt_restore(irq);
}
#endif
