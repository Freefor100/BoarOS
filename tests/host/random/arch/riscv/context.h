#ifndef RANDOM_TEST_CONTEXT_H
#define RANDOM_TEST_CONTEXT_H
#include <stdint.h>
static inline uintptr_t riscv_interrupt_save(void) { return 0; }
static inline void riscv_interrupt_restore(uintptr_t irq) { (void)irq; }
#endif
