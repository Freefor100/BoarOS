#ifndef BOAROS_ARCH_CONTEXT_H
#define BOAROS_ARCH_CONTEXT_H
#if defined(BOAROS_ARCH_LOONGARCH)
#include <arch/loongarch/context.h>
#else
#include <arch/riscv/context.h>
#define ARCH_INTERRUPT_ENABLE_MASK RISCV_SSTATUS_SIE
static inline uintptr_t arch_current_stack(void) { uintptr_t v; __asm__ volatile("mv %0, sp" : "=r"(v)); return v; }
static inline void arch_cpu_wait(void) { __asm__ volatile("wfi" ::: "memory"); }
#define arch_interrupt_save riscv_interrupt_save
#define arch_interrupt_restore riscv_interrupt_restore
#define arch_interrupt_is_enabled riscv_interrupt_is_enabled
#define arch_current_thread_get riscv_current_thread_get
#define arch_current_thread_set riscv_current_thread_set
#ifndef ARCH_TASK_CPU_OFFSET
#define ARCH_TASK_CPU_OFFSET 32
static inline void *arch_current_cpu_get(void)
{ void *v; __asm__ volatile("ld %0, 32(tp)" : "=r"(v)); return v; }
#endif
#endif
#endif
