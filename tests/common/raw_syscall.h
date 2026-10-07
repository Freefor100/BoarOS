#ifndef BOAROS_TEST_RAW_SYSCALL_H
#define BOAROS_TEST_RAW_SYSCALL_H
static inline long test_syscall6(long number,long a,long b,long c,long d,long e,long f)
{
    register long a0 __asm__("a0")=a,a1 __asm__("a1")=b,a2 __asm__("a2")=c;
    register long a3 __asm__("a3")=d,a4 __asm__("a4")=e,a5 __asm__("a5")=f,a7 __asm__("a7")=number;
#if defined(__loongarch__)
    __asm__ volatile("syscall 0":"+r"(a0):"r"(a1),"r"(a2),"r"(a3),"r"(a4),"r"(a5),"r"(a7):
        "$t0","$t1","$t2","$t3","$t4","$t5","$t6","$t7","$t8","memory");
#elif defined(__riscv)
    __asm__ volatile("ecall":"+r"(a0):"r"(a1),"r"(a2),"r"(a3),"r"(a4),"r"(a5),"r"(a7):"memory");
#else
#error "unsupported raw syscall architecture"
#endif
    return a0;
}
#endif
