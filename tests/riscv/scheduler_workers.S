    .section .text, "ax", @progbits

    .macro DEFINE_SCHEDULER_WORKER name, number, base
    .align 2
    .globl \name
    .type \name, @function
\name:
    addi sp, sp, -112
    sd ra, 0(sp)
    sd s0, 8(sp)
    sd s1, 16(sp)
    sd s2, 24(sp)
    sd s3, 32(sp)
    sd s4, 40(sp)
    sd s5, 48(sp)
    sd s6, 56(sp)
    sd s7, 64(sp)
    sd s8, 72(sp)
    sd s9, 80(sp)
    sd s10, 88(sp)
    sd s11, 96(sp)

    li a0, \number
    mv a1, sp
    mv a2, tp
    call scheduler_test_worker_started

    li s0, \base + 0
    li s1, \base + 1
    li s2, \base + 2
    li s3, \base + 3
    li s4, \base + 4
    li s5, \base + 5
    li s6, \base + 6
    li s7, \base + 7
    li s8, \base + 8
    li s9, \base + 9
    li s10, \base + 10
    li s11, \base + 11

1:
    la t0, scheduler_test_finish
    ld t1, 0(t0)
    beqz t1, 1b

    li t2, \base + 0
    bne s0, t2, 2f
    li t2, \base + 1
    bne s1, t2, 2f
    li t2, \base + 2
    bne s2, t2, 2f
    li t2, \base + 3
    bne s3, t2, 2f
    li t2, \base + 4
    bne s4, t2, 2f
    li t2, \base + 5
    bne s5, t2, 2f
    li t2, \base + 6
    bne s6, t2, 2f
    li t2, \base + 7
    bne s7, t2, 2f
    li t2, \base + 8
    bne s8, t2, 2f
    li t2, \base + 9
    bne s9, t2, 2f
    li t2, \base + 10
    bne s10, t2, 2f
    li t2, \base + 11
    beq s11, t2, 3f

2:
    la t0, scheduler_test_worker_failures
    ld t1, 0(t0)
    addi t1, t1, 1
    sd t1, 0(t0)

3:
    li a0, \number
    mv a1, sp
    mv a2, tp
    call scheduler_test_worker_resumed

    ld ra, 0(sp)
    ld s0, 8(sp)
    ld s1, 16(sp)
    ld s2, 24(sp)
    ld s3, 32(sp)
    ld s4, 40(sp)
    ld s5, 48(sp)
    ld s6, 56(sp)
    ld s7, 64(sp)
    ld s8, 72(sp)
    ld s9, 80(sp)
    ld s10, 88(sp)
    ld s11, 96(sp)
    addi sp, sp, 112
    ret

    .size \name, . - \name
    .endm

    DEFINE_SCHEDULER_WORKER scheduler_test_worker_a, 0, 0xa0
    DEFINE_SCHEDULER_WORKER scheduler_test_worker_b, 1, 0xb0
