#ifndef RANDOM_TEST_CONTEXT_H
#define RANDOM_TEST_CONTEXT_H
#include <stdint.h>
#define ARCH_TASK_CPU_OFFSET 32
void *allocator_host_cpu_get(void);
static inline void *arch_current_cpu_get(void) { return allocator_host_cpu_get(); }
static inline uintptr_t riscv_interrupt_save(void) { return 0; }
static inline void riscv_interrupt_restore(uintptr_t irq) { (void)irq; }
#endif
