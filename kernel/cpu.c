#include <kernel/cpu.h>
#include <kernel/raw_lock.h>

#if BOAROS_MAX_CPUS > 1
static unsigned boot_count;
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
