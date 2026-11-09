    .section .text.sigreturn,"ax",@progbits
    .globl __vdso_rt_sigreturn
    .type __vdso_rt_sigreturn,@function
__vdso_rt_sigreturn:
#ifdef __riscv
    li a7, 139
    ecall
#else
    li.w $a7, 139
    syscall 0
#endif
    .size __vdso_rt_sigreturn, . - __vdso_rt_sigreturn
