    .section .text.start, "ax", @progbits
    .align 2
    .globl _start
    .type _start, @function
_start:
    andi t0, sp, 15
    bnez t0, .Lfailure

    ld t0, 0(sp)
    li t1, 1
    bne t0, t1, .Lfailure
    ld t0, 8(sp)
    beqz t0, .Lfailure
    li t1, '/'
    lbu t2, 0(t0)
    bne t1, t2, .Lfailure
    li t1, 'i'
    lbu t2, 1(t0)
    bne t1, t2, .Lfailure
    li t1, 'n'
    lbu t2, 2(t0)
    bne t1, t2, .Lfailure
    li t1, 'i'
    lbu t2, 3(t0)
    bne t1, t2, .Lfailure
    li t1, 't'
    lbu t2, 4(t0)
    bne t1, t2, .Lfailure
    lbu t2, 5(t0)
    bnez t2, .Lfailure
    ld t0, 16(sp)
    bnez t0, .Lfailure
    ld t0, 24(sp)
    bnez t0, .Lfailure

    li a0, 0x2a
    li a7, 93
    ecall

.Lfailure:
    li a0, 0xe1
    li a7, 93
    ecall
    j .
    .size _start, . - _start
