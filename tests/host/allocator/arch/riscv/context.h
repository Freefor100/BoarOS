#ifndef ALLOCATOR_TEST_CONTEXT_H
#define ALLOCATOR_TEST_CONTEXT_H
#include <stdint.h>
extern uintptr_t allocator_test_irq_enabled;
static inline uintptr_t riscv_interrupt_save(void)
{
    uintptr_t old = allocator_test_irq_enabled;
    allocator_test_irq_enabled = 0;
    return old;
}
static inline void riscv_interrupt_restore(uintptr_t old)
{
    allocator_test_irq_enabled = old;
}
#endif
