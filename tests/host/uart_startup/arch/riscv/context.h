#ifndef BOAROS_UART_STARTUP_TEST_CONTEXT_H
#define BOAROS_UART_STARTUP_TEST_CONTEXT_H
#include <stdint.h>
extern unsigned fixture_irq_enabled;
static inline uintptr_t riscv_interrupt_save(void) {
    uintptr_t previous=fixture_irq_enabled?2:0;
    fixture_irq_enabled=0;return previous;
}
static inline void riscv_interrupt_restore(uintptr_t previous) {
    if(previous&2)fixture_irq_enabled=1;
}
#endif
