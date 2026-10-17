#ifndef BOAROS_SYNC_TEST_ARCH_CONTEXT_H
#define BOAROS_SYNC_TEST_ARCH_CONTEXT_H
#include <stdint.h>
#include <arch/riscv/context.h>
#define ARCH_TASK_CPU_OFFSET 32
#define ARCH_INTERRUPT_ENABLE_MASK RISCV_SSTATUS_SIE
void sync_test_current_task_set(void *);
uintptr_t sync_test_stack(void);
static inline void arch_current_thread_set(void *task) { sync_test_current_task_set(task); }
static inline uintptr_t arch_current_stack(void) { return sync_test_stack(); }
extern _Thread_local uintptr_t sync_test_irq;
void *sync_test_cpu(void);
void *sync_test_current_task(void);
static inline void *arch_current_thread_get(void) { return sync_test_current_task(); }
static inline void arch_cpu_wait(void) { __builtin_trap(); }
static inline uintptr_t arch_interrupt_save(void)
{ uintptr_t old = sync_test_irq; sync_test_irq = 0; return old; }
static inline void arch_interrupt_restore(uintptr_t old)
{
    sync_test_irq = old;
#ifdef BOAROS_TEST_IRQ_RESTORE
    extern _Thread_local unsigned sync_test_irq_restores;
    if (old) sync_test_irq_restores++;
#endif
}
static inline int arch_interrupt_is_enabled(void) { return sync_test_irq != 0; }
static inline void *arch_current_cpu_get(void) { return sync_test_cpu(); }
#endif
