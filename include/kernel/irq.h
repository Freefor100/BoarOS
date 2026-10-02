#ifndef BOAROS_KERNEL_IRQ_H
#define BOAROS_KERNEL_IRQ_H

#include <arch/riscv/context.h>

/* 单 hart 的短元数据区；不提供 SMP 互斥，也不阻止显式调度。 */
static inline void kernel_irq_scope_restore(uintptr_t *saved)
{
    riscv_interrupt_restore(*saved);
}
#define KERNEL_IRQ_SCOPE(name) \
    uintptr_t name __attribute__((cleanup(kernel_irq_scope_restore))) = \
        riscv_interrupt_save()

#endif
