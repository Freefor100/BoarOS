.section .text.start,"ax",@progbits
.global _start
_start:
.option push
.option norelax
    la gp, __global_pointer$
.option pop
    mv a0, sp
    call abi_main
    li a0, 99
    li a7, 93
    ecall
1:  j 1b

.global abi_clone_entry
abi_clone_entry:
    addi a1, a1, -16
    sd a3, 0(a1)
    sd a4, 8(a1)
    mv a4, a2
    li a2, 0
    li a3, 0
    li a7, 220
    ecall
    bnez a0, 2f
    ld t0, 0(sp)
    ld a0, 8(sp)
    jalr t0
    li a0, 0
    li a7, 93
    ecall
1:  j 1b
2:  ret
