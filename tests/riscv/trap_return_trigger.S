#include <arch/riscv/trap.h>

#define TEST_SNAPSHOT_STATUS 248
#define TEST_SAVED_RA 256
#define TEST_SAVED_GP 264
#define TEST_SAVED_TP 272
#define TEST_SAVED_S0 280
#define TEST_SAVED_S1 288
#define TEST_SAVED_S2 296
#define TEST_SAVED_S3 304
#define TEST_SAVED_S4 312
#define TEST_SAVED_S5 320
#define TEST_SAVED_S6 328
#define TEST_SAVED_S7 336
#define TEST_SAVED_S8 344
#define TEST_SAVED_S9 352
#define TEST_SAVED_S10 360
#define TEST_SAVED_S11 368
#define TEST_PROBE_FRAME_SIZE 384

    .section .text, "ax", @progbits
    .align 2

    .macro trap_test_save_caller
    addi sp, sp, -TEST_PROBE_FRAME_SIZE
    sd ra, TEST_SAVED_RA(sp)
    sd gp, TEST_SAVED_GP(sp)
    sd tp, TEST_SAVED_TP(sp)
    sd s0, TEST_SAVED_S0(sp)
    sd s1, TEST_SAVED_S1(sp)
    sd s2, TEST_SAVED_S2(sp)
    sd s3, TEST_SAVED_S3(sp)
    sd s4, TEST_SAVED_S4(sp)
    sd s5, TEST_SAVED_S5(sp)
    sd s6, TEST_SAVED_S6(sp)
    sd s7, TEST_SAVED_S7(sp)
    sd s8, TEST_SAVED_S8(sp)
    sd s9, TEST_SAVED_S9(sp)
    sd s10, TEST_SAVED_S10(sp)
    sd s11, TEST_SAVED_S11(sp)
    .endm

    .macro trap_test_load_sentinels
    li ra, 0x0101010181010101
    li tp, 0x0404040484040404
    li t0, 0x0505050585050505
    li t1, 0x0606060686060606
    li t2, 0x0707070787070707
    li s0, 0x0808080888080808
    li s1, 0x0909090989090909
    li a0, 0x0a0a0a0a8a0a0a0a
    li a1, 0x0b0b0b0b8b0b0b0b
    li a2, 0x0c0c0c0c8c0c0c0c
    li a3, 0x0d0d0d0d8d0d0d0d
    li a4, 0x0e0e0e0e8e0e0e0e
    li a5, 0x0f0f0f0f8f0f0f0f
    li a6, 0x1010101090101010
    li a7, 0x1111111191111111
    li s2, 0x1212121292121212
    li s3, 0x1313131393131313
    li s4, 0x1414141494141414
    li s5, 0x1515151595151515
    li s6, 0x1616161696161616
    li s7, 0x1717171797171717
    li s8, 0x1818181898181818
    li s9, 0x1919191999191919
    li s10, 0x1a1a1a1a9a1a1a1a
    li s11, 0x1b1b1b1b9b1b1b1b
    li t3, 0x1c1c1c1c9c1c1c1c
    li t4, 0x1d1d1d1d9d1d1d1d
    li t5, 0x1e1e1e1e9e1e1e1e
    li t6, 0x1f1f1f1f9f1f1f1f
    .endm

    .macro trap_test_snapshot
    sd ra, RISCV_TRAP_FRAME_RA(sp)
    sd sp, RISCV_TRAP_FRAME_SP(sp)
    sd gp, RISCV_TRAP_FRAME_GP(sp)
    sd tp, RISCV_TRAP_FRAME_TP(sp)
    sd t0, RISCV_TRAP_FRAME_T0(sp)
    sd t1, RISCV_TRAP_FRAME_T1(sp)
    sd t2, RISCV_TRAP_FRAME_T2(sp)
    sd s0, RISCV_TRAP_FRAME_S0(sp)
    sd s1, RISCV_TRAP_FRAME_S1(sp)
    sd a0, RISCV_TRAP_FRAME_A0(sp)
    sd a1, RISCV_TRAP_FRAME_A1(sp)
    sd a2, RISCV_TRAP_FRAME_A2(sp)
    sd a3, RISCV_TRAP_FRAME_A3(sp)
    sd a4, RISCV_TRAP_FRAME_A4(sp)
    sd a5, RISCV_TRAP_FRAME_A5(sp)
    sd a6, RISCV_TRAP_FRAME_A6(sp)
    sd a7, RISCV_TRAP_FRAME_A7(sp)
    sd s2, RISCV_TRAP_FRAME_S2(sp)
    sd s3, RISCV_TRAP_FRAME_S3(sp)
    sd s4, RISCV_TRAP_FRAME_S4(sp)
    sd s5, RISCV_TRAP_FRAME_S5(sp)
    sd s6, RISCV_TRAP_FRAME_S6(sp)
    sd s7, RISCV_TRAP_FRAME_S7(sp)
    sd s8, RISCV_TRAP_FRAME_S8(sp)
    sd s9, RISCV_TRAP_FRAME_S9(sp)
    sd s10, RISCV_TRAP_FRAME_S10(sp)
    sd s11, RISCV_TRAP_FRAME_S11(sp)
    sd t3, RISCV_TRAP_FRAME_T3(sp)
    sd t4, RISCV_TRAP_FRAME_T4(sp)
    sd t5, RISCV_TRAP_FRAME_T5(sp)
    sd t6, RISCV_TRAP_FRAME_T6(sp)
    csrrci t6, sstatus, 2
    sd t6, TEST_SNAPSHOT_STATUS(sp)
    .endm

    .macro trap_test_check_immediate offset, expected
    ld t3, \offset(sp)
    li t4, \expected
    beq t3, t4, 1f
    addi a0, a0, 1
1:
    .endm

    .macro trap_test_check_snapshot expected_sie
    li a0, 0
    trap_test_check_immediate RISCV_TRAP_FRAME_RA, 0x0101010181010101
    ld t3, RISCV_TRAP_FRAME_SP(sp)
    beq t3, sp, 1f
    addi a0, a0, 1
1:
    ld t3, RISCV_TRAP_FRAME_GP(sp)
    ld t4, TEST_SAVED_GP(sp)
    beq t3, t4, 1f
    addi a0, a0, 1
1:
    trap_test_check_immediate RISCV_TRAP_FRAME_TP, 0x0404040484040404
    trap_test_check_immediate RISCV_TRAP_FRAME_T0, 0x0505050585050505
    trap_test_check_immediate RISCV_TRAP_FRAME_T1, 0x0606060686060606
    trap_test_check_immediate RISCV_TRAP_FRAME_T2, 0x0707070787070707
    trap_test_check_immediate RISCV_TRAP_FRAME_S0, 0x0808080888080808
    trap_test_check_immediate RISCV_TRAP_FRAME_S1, 0x0909090989090909
    trap_test_check_immediate RISCV_TRAP_FRAME_A0, 0x0a0a0a0a8a0a0a0a
    trap_test_check_immediate RISCV_TRAP_FRAME_A1, 0x0b0b0b0b8b0b0b0b
    trap_test_check_immediate RISCV_TRAP_FRAME_A2, 0x0c0c0c0c8c0c0c0c
    trap_test_check_immediate RISCV_TRAP_FRAME_A3, 0x0d0d0d0d8d0d0d0d
    trap_test_check_immediate RISCV_TRAP_FRAME_A4, 0x0e0e0e0e8e0e0e0e
    trap_test_check_immediate RISCV_TRAP_FRAME_A5, 0x0f0f0f0f8f0f0f0f
    trap_test_check_immediate RISCV_TRAP_FRAME_A6, 0x1010101090101010
    trap_test_check_immediate RISCV_TRAP_FRAME_A7, 0x1111111191111111
    trap_test_check_immediate RISCV_TRAP_FRAME_S2, 0x1212121292121212
    trap_test_check_immediate RISCV_TRAP_FRAME_S3, 0x1313131393131313
    trap_test_check_immediate RISCV_TRAP_FRAME_S4, 0x1414141494141414
    trap_test_check_immediate RISCV_TRAP_FRAME_S5, 0x1515151595151515
    trap_test_check_immediate RISCV_TRAP_FRAME_S6, 0x1616161696161616
    trap_test_check_immediate RISCV_TRAP_FRAME_S7, 0x1717171797171717
    trap_test_check_immediate RISCV_TRAP_FRAME_S8, 0x1818181898181818
    trap_test_check_immediate RISCV_TRAP_FRAME_S9, 0x1919191999191919
    trap_test_check_immediate RISCV_TRAP_FRAME_S10, 0x1a1a1a1a9a1a1a1a
    trap_test_check_immediate RISCV_TRAP_FRAME_S11, 0x1b1b1b1b9b1b1b1b
    trap_test_check_immediate RISCV_TRAP_FRAME_T3, 0x1c1c1c1c9c1c1c1c
    trap_test_check_immediate RISCV_TRAP_FRAME_T4, 0x1d1d1d1d9d1d1d1d
    trap_test_check_immediate RISCV_TRAP_FRAME_T5, 0x1e1e1e1e9e1e1e1e
    trap_test_check_immediate RISCV_TRAP_FRAME_T6, 0x1f1f1f1f9f1f1f1f
    ld t3, TEST_SNAPSHOT_STATUS(sp)
    andi t3, t3, RISCV_SSTATUS_SIE
    li t4, \expected_sie
    beq t3, t4, 1f
    addi a0, a0, 1
1:
    .endm

    .macro trap_test_restore_caller
    ld ra, TEST_SAVED_RA(sp)
    ld gp, TEST_SAVED_GP(sp)
    ld tp, TEST_SAVED_TP(sp)
    ld s0, TEST_SAVED_S0(sp)
    ld s1, TEST_SAVED_S1(sp)
    ld s2, TEST_SAVED_S2(sp)
    ld s3, TEST_SAVED_S3(sp)
    ld s4, TEST_SAVED_S4(sp)
    ld s5, TEST_SAVED_S5(sp)
    ld s6, TEST_SAVED_S6(sp)
    ld s7, TEST_SAVED_S7(sp)
    ld s8, TEST_SAVED_S8(sp)
    ld s9, TEST_SAVED_S9(sp)
    ld s10, TEST_SAVED_S10(sp)
    ld s11, TEST_SAVED_S11(sp)
    addi sp, sp, TEST_PROBE_FRAME_SIZE
    ret
    .endm

    .globl trap_test_breakpoint_return
    .type trap_test_breakpoint_return, @function
trap_test_breakpoint_return:
    trap_test_save_caller
    trap_test_load_sentinels
    .globl trap_test_recoverable_breakpoint
trap_test_recoverable_breakpoint:
    .4byte 0x00100073
    trap_test_snapshot
    trap_test_check_snapshot 0
    trap_test_restore_caller
    .size trap_test_breakpoint_return, . - trap_test_breakpoint_return

    .globl trap_test_software_interrupt_return
    .type trap_test_software_interrupt_return, @function
trap_test_software_interrupt_return:
    trap_test_save_caller
    trap_test_load_sentinels
    csrsi sie, 2
    csrsi sip, 2
    csrsi sstatus, 2
    .globl trap_test_software_interrupt_resume
trap_test_software_interrupt_resume:
    trap_test_snapshot
    csrci sie, 2
    csrci sip, 2
    trap_test_check_snapshot RISCV_SSTATUS_SIE
    trap_test_restore_caller
    .size trap_test_software_interrupt_return, . - trap_test_software_interrupt_return

    .globl trap_test_bad_return
    .type trap_test_bad_return, @function
trap_test_bad_return:
    addi sp, sp, -16
    sd ra, 8(sp)
    .globl trap_test_bad_return_breakpoint
trap_test_bad_return_breakpoint:
    .4byte 0x00100073
    ld ra, 8(sp)
    addi sp, sp, 16
    ret
    .size trap_test_bad_return, . - trap_test_bad_return
