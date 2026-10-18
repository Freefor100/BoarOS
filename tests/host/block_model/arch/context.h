#ifndef BOAROS_BLOCK_MODEL_CONTEXT_H
#define BOAROS_BLOCK_MODEL_CONTEXT_H
#include <stdint.h>
#ifndef BOAROS_PAGE_SHIFT
#define BOAROS_PAGE_SHIFT 12
#endif
static inline void *arch_current_cpu_get(void) { return (void *)0; }
/* Standalone block fixtures execute host threads, without guest interrupts.
 * Device exclusion is provided by block_runtime.c's blocking mutex model. */
static inline uintptr_t arch_interrupt_save(void) { return 0; }
static inline void arch_interrupt_restore(uintptr_t state) { (void)state; }
#endif
