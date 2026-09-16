.section .text.start,"ax",@progbits
.global _start
_start:
.option push
.option norelax
    la gp, __global_pointer$
.option pop
    call abi_main
    li a0, 99
    li a7, 93
    ecall
1:  j 1b
