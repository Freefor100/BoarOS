#include <kernel/cpu.h>
#include <kernel/raw_lock.h>

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
