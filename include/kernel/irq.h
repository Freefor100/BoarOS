#ifndef BOAROS_KERNEL_IRQ_H
#define BOAROS_KERNEL_IRQ_H

#include <arch/context.h>

/* 只保存/恢复本CPU的中断状态；不提供SMP互斥，也不阻止显式调度。
 * 与它不同：抢占/迁移控制约束CPU本地状态的借用；跨核raw锁保护短共享区且
 * 禁止阻塞；可睡眠对象锁由任务guard持有。后三者不能由IRQ scope推导。
 * 当前wait/压力路径可在IRQ关闭时主动切走，迁移SMP时不可机械换成raw锁。 */
static inline void kernel_irq_scope_restore(uintptr_t *saved)
{
    arch_interrupt_restore(*saved);
}
#define KERNEL_IRQ_SCOPE(name) \
    uintptr_t name __attribute__((cleanup(kernel_irq_scope_restore))) = \
        arch_interrupt_save()

#endif
