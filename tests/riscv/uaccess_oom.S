    .section .text.start, "ax", @progbits
    .align 2
    .globl _start
    .type _start, @function
_start:
    # Reserve far more demand-zero memory than this 64 MiB test machine can
    # instantiate.  uname writes through the real uaccess path, so exhausting
    # a later page must return EFAULT rather than corrupting kernel control.
    li a0, 0
    li a1, 0x08000000
    li a2, 3
    li a3, 0x22
    li a4, -1
    li a5, 0
    li a7, 222
    ecall
    bltz a0, .Lfailure
    mv s0, a0
    li s1, 0x8000

.Ltouch:
    mv a0, s0
    li a7, 160
    ecall
    beqz a0, .Lnext
    li t0, -14
    bne a0, t0, .Lfailure
    # The failed page is still a legal registered robust-list address.
    # Exit must tolerate another user fault while memory is exhausted.
    mv a0, s0
    li a1, 24
    li a7, 99
    ecall
    bnez a0, .Lfailure
    li a0, 42
    li a7, 93
    ecall

.Lnext:
    li t0, 0x1000
    add s0, s0, t0
    addi s1, s1, -1
    bnez s1, .Ltouch

.Lfailure:
    li a0, 1
    li a7, 93
    ecall
