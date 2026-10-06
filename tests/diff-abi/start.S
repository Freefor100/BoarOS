.section .text.start,"ax",@progbits
.global _start
_start:
#if defined(__loongarch__)
    move $a0, $sp
    bl abi_main
    li.w $a0, 99
    li.w $a7, 93
    syscall 0
1:  b 1b
#else
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

#endif
.global abi_clone_entry
abi_clone_entry:
#if defined(__loongarch__)
    addi.d $a1, $a1, -16
    st.d $a3, $a1, 0
    st.d $a4, $a1, 8
    move $a3, $a2
    move $a2, $zero
    move $a4, $zero
    li.w $a7, 220
    syscall 0
    bnez $a0, 2f
    ld.d $t0, $sp, 0
    ld.d $a0, $sp, 8
    jirl $ra, $t0, 0
    li.w $a0, 0
    li.w $a7, 93
    syscall 0
1:  b 1b
2:  jr $ra
#else
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

#endif
