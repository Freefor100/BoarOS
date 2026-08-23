#include <arch/riscv/context.h>

    .section .text, "ax", @progbits
    .align 2
    .globl riscv_context_switch
    .type riscv_context_switch, @function

riscv_context_switch:
    sd ra, RISCV_SWITCH_CONTEXT_RA(a0)
    sd sp, RISCV_SWITCH_CONTEXT_SP(a0)
    sd tp, RISCV_SWITCH_CONTEXT_TP(a0)
    sd s0, RISCV_SWITCH_CONTEXT_S0(a0)
    sd s1, RISCV_SWITCH_CONTEXT_S1(a0)
    sd s2, RISCV_SWITCH_CONTEXT_S2(a0)
    sd s3, RISCV_SWITCH_CONTEXT_S3(a0)
    sd s4, RISCV_SWITCH_CONTEXT_S4(a0)
    sd s5, RISCV_SWITCH_CONTEXT_S5(a0)
    sd s6, RISCV_SWITCH_CONTEXT_S6(a0)
    sd s7, RISCV_SWITCH_CONTEXT_S7(a0)
    sd s8, RISCV_SWITCH_CONTEXT_S8(a0)
    sd s9, RISCV_SWITCH_CONTEXT_S9(a0)
    sd s10, RISCV_SWITCH_CONTEXT_S10(a0)
    sd s11, RISCV_SWITCH_CONTEXT_S11(a0)

    ld ra, RISCV_SWITCH_CONTEXT_RA(a1)
    ld sp, RISCV_SWITCH_CONTEXT_SP(a1)
    ld tp, RISCV_SWITCH_CONTEXT_TP(a1)
    ld s0, RISCV_SWITCH_CONTEXT_S0(a1)
    ld s1, RISCV_SWITCH_CONTEXT_S1(a1)
    ld s2, RISCV_SWITCH_CONTEXT_S2(a1)
    ld s3, RISCV_SWITCH_CONTEXT_S3(a1)
    ld s4, RISCV_SWITCH_CONTEXT_S4(a1)
    ld s5, RISCV_SWITCH_CONTEXT_S5(a1)
    ld s6, RISCV_SWITCH_CONTEXT_S6(a1)
    ld s7, RISCV_SWITCH_CONTEXT_S7(a1)
    ld s8, RISCV_SWITCH_CONTEXT_S8(a1)
    ld s9, RISCV_SWITCH_CONTEXT_S9(a1)
    ld s10, RISCV_SWITCH_CONTEXT_S10(a1)
    ld s11, RISCV_SWITCH_CONTEXT_S11(a1)
    ret

    .size riscv_context_switch, . - riscv_context_switch

    .align 2
    .globl riscv_kernel_thread_trampoline
    .type riscv_kernel_thread_trampoline, @function

riscv_kernel_thread_trampoline:
    csrsi sstatus, RISCV_SSTATUS_SIE
    mv a0, s1
    jalr s0
    csrci sstatus, RISCV_SSTATUS_SIE
    tail kernel_thread_exit

    .size riscv_kernel_thread_trampoline, \
          . - riscv_kernel_thread_trampoline
