#ifndef BOAROS_SYNC_TEST_ARCH_CONTEXT_H
#define BOAROS_SYNC_TEST_ARCH_CONTEXT_H
#include <stdint.h>
#include <arch/riscv/context.h>
extern _Thread_local uintptr_t sync_test_irq;
void *sync_test_cpu(void);
static inline uintptr_t arch_interrupt_save(void)
{ uintptr_t old = sync_test_irq; sync_test_irq = 0; return old; }
static inline void arch_interrupt_restore(uintptr_t old) { sync_test_irq = old; }
static inline int arch_interrupt_is_enabled(void) { return sync_test_irq != 0; }
static inline void *arch_current_cpu_get(void) { return sync_test_cpu(); }
#endif
