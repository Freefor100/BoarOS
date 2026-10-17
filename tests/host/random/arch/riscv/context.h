#ifndef RANDOM_TEST_CONTEXT_H
#define RANDOM_TEST_CONTEXT_H
#include <stdint.h>
#define ARCH_TASK_CPU_OFFSET 32
void *allocator_host_cpu_get(void);
static inline void *arch_current_cpu_get(void) { return allocator_host_cpu_get(); }
#define RISCV_SSTATUS_SIE 2U
#ifdef BOAROS_HOST_IRQ_STATE
extern _Thread_local uintptr_t network_host_irq;
#ifdef BOAROS_HOST_IRQ_COUNTS
extern _Thread_local unsigned network_host_irq_saves,network_host_irq_restores;
#endif
static inline uintptr_t riscv_interrupt_save(void)
{
#ifdef BOAROS_HOST_IRQ_COUNTS
    network_host_irq_saves++;
#endif
    uintptr_t old = network_host_irq; network_host_irq = 0; return old;
}
static inline void riscv_interrupt_restore(uintptr_t irq)
{
#ifdef BOAROS_HOST_IRQ_COUNTS
    network_host_irq_restores++;
#endif
    network_host_irq = irq;
}
static inline int riscv_interrupt_is_enabled(void) { return !!network_host_irq; }
#else
static inline uintptr_t riscv_interrupt_save(void) { return 0; }
static inline void riscv_interrupt_restore(uintptr_t irq) { (void)irq; }
static inline int riscv_interrupt_is_enabled(void) { return 0; }
#endif
#endif
