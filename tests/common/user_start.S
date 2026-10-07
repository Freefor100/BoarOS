    .text
    .globl _start
_start:
#if defined(__loongarch__)
    move $a0, $sp
    bl user_main
    li.w $a7, 93
    syscall 0
1:  b 1b
#elif defined(__riscv)
    mv a0, sp
    call user_main
    li a7, 93
    ecall
1:  j 1b
#else
#error "unsupported startup architecture"
#endif
