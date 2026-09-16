    .section .text.start, "ax", @progbits
    .align 2
    .globl _start
_start:
    # Keep SIGCHLD pending so the parent sleeps until the child has exited.
    li a0, 0
    la a1, child_mask
    li a2, 0
    li a3, 8
    li a7, 135
    ecall
    bnez a0, failure

    # clone(SIGCHLD, NULL) is fork; leave the child as an unwaited zombie.
    li a0, 17
    li a1, 0
    li a2, 0
    li a3, 0
    li a4, 0
    li a7, 220
    ecall
    bltz a0, failure
    beqz a0, child

    la a0, delay
    li a1, 0
    li a7, 101
    ecall
    bnez a0, failure
    li a0, 42
    li a7, 93
    ecall

child:
    li a0, 0
    li a7, 93
    ecall

failure:
    li a0, 1
    li a7, 93
    ecall

    .section .rodata
    .align 3
child_mask:
    .dword 0x10000
delay:
    .dword 0
    .dword 100000000
