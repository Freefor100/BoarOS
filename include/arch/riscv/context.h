#ifndef BOAROS_ARCH_RISCV_CONTEXT_H
#define BOAROS_ARCH_RISCV_CONTEXT_H

#define RISCV_SWITCH_CONTEXT_RA 0
#define RISCV_SWITCH_CONTEXT_SP 8
#define RISCV_SWITCH_CONTEXT_TP 16
#define RISCV_SWITCH_CONTEXT_S0 24
#define RISCV_SWITCH_CONTEXT_S1 32
#define RISCV_SWITCH_CONTEXT_S2 40
#define RISCV_SWITCH_CONTEXT_S3 48
#define RISCV_SWITCH_CONTEXT_S4 56
#define RISCV_SWITCH_CONTEXT_S5 64
#define RISCV_SWITCH_CONTEXT_S6 72
#define RISCV_SWITCH_CONTEXT_S7 80
#define RISCV_SWITCH_CONTEXT_S8 88
#define RISCV_SWITCH_CONTEXT_S9 96
#define RISCV_SWITCH_CONTEXT_S10 104
#define RISCV_SWITCH_CONTEXT_S11 112
#define RISCV_SWITCH_CONTEXT_SIZE 128

#define RISCV_SSTATUS_SIE 0x2
#define RISCV_CPU_COST_OFFSET 32
#define RISCV_COST_TRAP_TIMESTAMP 0
#define RISCV_COST_RETURN_TIMESTAMP 8
#define RISCV_COST_RETURN_PENDING 16

#ifndef __ASSEMBLER__

#include <stddef.h>
#include <stdint.h>
#include <kernel/cost.h>
#include <arch/switch_context.h>

enum riscv_context_status {
    RISCV_CONTEXT_STATUS_OK = 0,
    RISCV_CONTEXT_STATUS_INVALID_ARGUMENT,
};

#define RISCV_CONTEXT_ASSERT_OFFSET(member, offset) \
    _Static_assert(offsetof(struct riscv_switch_context, member) == (offset), \
                   "RISC-V switch context offset mismatch: " #member)

_Static_assert(sizeof(uint64_t) == 8U,
               "RISC-V switch context requires RV64");
RISCV_CONTEXT_ASSERT_OFFSET(ra, RISCV_SWITCH_CONTEXT_RA);
RISCV_CONTEXT_ASSERT_OFFSET(sp, RISCV_SWITCH_CONTEXT_SP);
RISCV_CONTEXT_ASSERT_OFFSET(tp, RISCV_SWITCH_CONTEXT_TP);
RISCV_CONTEXT_ASSERT_OFFSET(s[0], RISCV_SWITCH_CONTEXT_S0);
RISCV_CONTEXT_ASSERT_OFFSET(s[1], RISCV_SWITCH_CONTEXT_S1);
RISCV_CONTEXT_ASSERT_OFFSET(s[2], RISCV_SWITCH_CONTEXT_S2);
RISCV_CONTEXT_ASSERT_OFFSET(s[3], RISCV_SWITCH_CONTEXT_S3);
RISCV_CONTEXT_ASSERT_OFFSET(s[4], RISCV_SWITCH_CONTEXT_S4);
RISCV_CONTEXT_ASSERT_OFFSET(s[5], RISCV_SWITCH_CONTEXT_S5);
RISCV_CONTEXT_ASSERT_OFFSET(s[6], RISCV_SWITCH_CONTEXT_S6);
RISCV_CONTEXT_ASSERT_OFFSET(s[7], RISCV_SWITCH_CONTEXT_S7);
RISCV_CONTEXT_ASSERT_OFFSET(s[8], RISCV_SWITCH_CONTEXT_S8);
RISCV_CONTEXT_ASSERT_OFFSET(s[9], RISCV_SWITCH_CONTEXT_S9);
RISCV_CONTEXT_ASSERT_OFFSET(s[10], RISCV_SWITCH_CONTEXT_S10);
RISCV_CONTEXT_ASSERT_OFFSET(s[11], RISCV_SWITCH_CONTEXT_S11);
_Static_assert(sizeof(struct riscv_switch_context) ==
                   RISCV_SWITCH_CONTEXT_SIZE,
               "RISC-V switch context size mismatch");
_Static_assert(_Alignof(struct riscv_switch_context) == 16U,
               "RISC-V switch context alignment mismatch");

#undef RISCV_CONTEXT_ASSERT_OFFSET

enum riscv_context_status riscv_context_init(
    struct riscv_switch_context *context,
    uintptr_t stack_top,
    void (*entry)(void *),
    void *argument,
    void *thread_pointer);

enum riscv_context_status riscv_context_init_user(
    struct riscv_switch_context *context,
    uintptr_t trap_frame_pointer,
    void *thread_pointer);

void riscv_context_switch(
    struct riscv_switch_context *previous,
    const struct riscv_switch_context *next);

static inline uintptr_t riscv_interrupt_save(void)
{
    uintptr_t old_status;
    uintptr_t mask = RISCV_SSTATUS_SIE;

    __asm__ volatile("csrrc %0, sstatus, %1"
                     : "=r"(old_status)
                     : "r"(mask)
                     : "memory");
#if BOAROS_COST_DIAGNOSTICS
    if (old_status & RISCV_SSTATUS_SIE) {
        uint64_t ticks; __asm__ volatile("rdtime %0" : "=r"(ticks));
        kernel_cost_irq_disabled(ticks);
    }
#endif
    return old_status;
}

static inline void riscv_interrupt_restore(uintptr_t old_status)
{
    if ((old_status & RISCV_SSTATUS_SIE) != 0U) {
#if BOAROS_COST_DIAGNOSTICS
        kernel_cost_irq_enable_now();
#endif
        __asm__ volatile("csrsi sstatus, 2" ::: "memory");
    }
}

static inline int riscv_interrupt_is_enabled(void)
{
    uintptr_t status;

    __asm__ volatile("csrr %0, sstatus" : "=r"(status));
    return (status & RISCV_SSTATUS_SIE) != 0U;
}

#if defined(__riscv)
/* TP names the executing stack owner. Giving it a C register identity lets
 * embedded-state reads share that owner without repeated asm moves. */
register void *riscv_current_is_tp __asm__("tp");

static inline void riscv_current_thread_set(void *thread)
{
    riscv_current_is_tp = thread;
    __asm__ volatile("" : : "r"(riscv_current_is_tp) : "memory");
}

static inline void *riscv_current_thread_get(void)
{
    return riscv_current_is_tp;
}
#endif

#endif

#endif
