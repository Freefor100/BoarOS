#ifndef BOAROS_ARCH_LOONGARCH_CONTEXT_H
#define BOAROS_ARCH_LOONGARCH_CONTEXT_H
#define ARCH_INTERRUPT_ENABLE_MASK 4
#define ARCH_TASK_CPU_OFFSET 48
#ifndef __ASSEMBLER__
#include <stdint.h>
static inline uintptr_t arch_current_stack(void) { uintptr_t v; __asm__ volatile("move %0, $sp" : "=r"(v)); return v; }
static inline void arch_cpu_wait(void) { __asm__ volatile("idle 0" ::: "memory"); }
static inline uintptr_t arch_interrupt_save(void)
{
    uintptr_t old, mask = 4;
    __asm__ volatile("csrxchg %0, %1, 0" : "=r"(old) : "r"(mask), "0"((uintptr_t)0) : "memory");
    return old;
}
static inline void arch_interrupt_restore(uintptr_t old)
{
    uintptr_t mask = 4;
    __asm__ volatile("csrxchg %0, %1, 0" : "+r"(old) : "r"(mask) : "memory");
}
static inline int arch_interrupt_is_enabled(void)
{ uintptr_t v; __asm__ volatile("csrrd %0, 0" : "=r"(v)); return (v & 4) != 0; }
static inline void *arch_current_thread_get(void)
{ void *v; __asm__ volatile("move %0, $tp" : "=r"(v)); return v; }
static inline void arch_current_thread_set(void *v)
{ __asm__ volatile("move $tp, %0" :: "r"(v) : "memory"); }
static inline void *arch_current_cpu_get(void)
{ void *v; __asm__ volatile("ld.d %0, $tp, 48" : "=r"(v)); return v; }
#endif
#endif
