#include "user_test.h"

    .section .rodata.user_test_payload, "a", @progbits
    .align 2
    .globl user_test_payload_start
user_test_payload_start:
    li gp, USER_TEST_GP_VALUE
    li s0, 0x101
    li s1, 0x102
    li s2, 0x103
    li s3, 0x104
    li s4, 0x105
    li s5, 0x106
    li s6, 0x107
    li s7, 0x108
    li s8, 0x109
    li s9, 0x10a
    li s10, USER_TEST_DATA_VA
    li s11, 0x10c

    li t0, USER_TEST_STARTED_VALUE
    sd t0, USER_TEST_STARTED_OFFSET(s10)
1:
    ld t0, USER_TEST_FLAG_OFFSET(s10)
    beqz t0, 1b

    li t0, USER_TEST_TP_VALUE
    beq tp, t0, 4f
    li t0, USER_TEST_SECOND_TP_VALUE
    bne tp, t0, 2f
    li t0, USER_TEST_SECOND_STACK_TOP
    j 5f
4:
    li t0, USER_TEST_STACK_TOP
5:
    bne sp, t0, 2f
    li t0, USER_TEST_GP_VALUE
    bne gp, t0, 2f
    li t0, 0x101
    bne s0, t0, 2f
    li t0, 0x102
    bne s1, t0, 2f
    li t0, 0x103
    bne s2, t0, 2f
    li t0, 0x104
    bne s3, t0, 2f
    li t0, 0x105
    bne s4, t0, 2f
    li t0, 0x106
    bne s5, t0, 2f
    li t0, 0x107
    bne s6, t0, 2f
    li t0, 0x108
    bne s7, t0, 2f
    li t0, 0x109
    bne s8, t0, 2f
    li t0, 0x10a
    bne s9, t0, 2f
    li t0, USER_TEST_DATA_VA
    bne s10, t0, 2f
    li t0, 0x10c
    bne s11, t0, 2f
    li a7, 0x7fff
    ecall
    li t0, -38
    bne a0, t0, 2f
    li t0, USER_TEST_GP_VALUE
    bne gp, t0, 2f

    # Low-level U-mode probes deliberately have no files/fs owners.
    li a0, -100
    li a1, USER_TEST_DATA_VA
    li a2, 0
    li a3, 0
    li a7, 56
    ecall
    li t0, -19
    bne a0, t0, 2f
    li a0, 0
    li a1, USER_TEST_DATA_VA
    li a2, 1
    li a7, 63
    ecall
    li t0, -9
    bne a0, t0, 2f
    li a0, 0
    li a7, 57
    ecall
    li t0, -9
    bne a0, t0, 2f

    li a7, 172
    ecall
    blez a0, 2f
    mv s0, a0

    li a7, 178
    ecall
    blez a0, 2f
    bne a0, s0, 2f

    li t0, 1
    addi t1, s10, USER_TEST_ID_COUNT_OFFSET
    amoadd.d t2, t0, (t1)
    li t0, 2
    bgeu t2, t0, 2f
    slli t2, t2, 4
    addi t2, t2, USER_TEST_ID_RECORDS_OFFSET
    add t2, s10, t2
    sd s0, 0(t2)
    sd a0, 8(t2)

    li a0, USER_TEST_UNAME_ADDRESS
    li a7, 160
    ecall
    bnez a0, 2f

    li a0, 0
    li a7, 160
    ecall
    li t0, -14
    bne a0, t0, 2f

    li a0, USER_TEST_CODE_VA
    li a7, 160
    ecall
    li t0, -14
    bne a0, t0, 2f

    li a0, USER_TEST_USER_LIMIT - 1
    li a7, 160
    ecall
    li t0, -14
    bne a0, t0, 2f

    li t0, USER_TEST_RESUMED_VALUE
    sd t0, USER_TEST_RESUMED_OFFSET(s10)
    li a0, USER_TEST_EXIT_STATUS
    li a7, 93
    ecall

2:
    li t0, USER_TEST_DATA_VA
    ld t1, USER_TEST_FAILURES_OFFSET(t0)
    addi t1, t1, 1
    sd t1, USER_TEST_FAILURES_OFFSET(t0)
    li a0, 0xe1
    li a7, 93
    ecall
3:
    j 3b

    .globl user_test_payload_end
user_test_payload_end:

    .align 2
    .globl user_test_fault_payload_start
user_test_fault_payload_start:
    ld t0, 0(zero)
1:
    j 1b
    .globl user_test_fault_payload_end
user_test_fault_payload_end:
