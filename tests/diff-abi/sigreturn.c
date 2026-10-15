#include "abi.h"

struct return_action { unsigned long handler, flags, mask; };
static volatile unsigned mode, handler_seen;

static void return_handler(int signal, void *info, void *context)
{
    (void)info;
    if (signal != 10) abi_exit(90);
    handler_seen++;
    unsigned long base = (unsigned long)context - 128;
    unsigned long target = base;
    if (mode == 1) {
#if defined(__loongarch__)
        const unsigned frame_size = 592; /* Integer frame plus END record. */
#else
        const unsigned frame_size = 1088; /* Integer and reserved FP union. */
#endif
        target -= 8;
        volatile unsigned char *from = (void *)base, *to = (void *)target;
        for (unsigned i = 0; i < frame_size; i++) to[i] = from[i];
    } else if (mode == 2) target = (unsigned long)-16;
#if defined(__loongarch__)
    __asm__ volatile("move $sp,%0;li.w $a7,139;syscall 0" :: "r"(target) : "$a7", "memory");
#else
    __asm__ volatile("mv sp,%0;li a7,139;ecall" :: "r"(target) : "a7", "memory");
#endif
    __builtin_unreachable();
}

void abi_sigreturn_cases(void)
{
    const char *ids[] = {"boundary.sigreturn-aligned", "boundary.sigreturn-offset8",
        "boundary.sigreturn-unreadable"};
    for (mode = 0; mode < 3; mode++) {
        long child = SC5(220, 17, 0, 0, 0, 0);
        abi_require(child >= 0);
        if (!child) {
            handler_seen = 0;
            struct return_action action = {(unsigned long)return_handler, 4, 0};
            unsigned long blocked = 1UL << 11, restored = 0;
            abi_require(SC4(135, 2, &blocked, 0, 8) == 0);
            abi_require(SC4(134, 10, &action, 0, 8) == 0);
#if defined(__riscv_flen) && __riscv_flen >= 64
            unsigned long expected = 0x400921fb54442d18UL, value, fcsr;
            __asm__ volatile("fmv.d.x f31,%0;csrw fcsr,%1" :: "r"(expected), "r"(32UL) : "f31", "memory");
#endif
            abi_require(SC2(129, SC0(172), 10) == 0);
#if defined(__riscv_flen) && __riscv_flen >= 64
            __asm__ volatile("fmv.x.d %0,f31;csrr %1,fcsr" : "=r"(value), "=r"(fcsr) :: "memory");
            if (value != expected || fcsr != 32) abi_exit(91);
#endif
            if (SC4(135, 2, 0, &restored, 8) || restored != blocked || handler_seen != 1)
                abi_exit(92);
            abi_exit(0);
        }
        int status = 0;
        abi_require(SC4(260, child, &status, 0, 0) == child);
        abi_record(ids[mode], 0, -1, -1, status, 0, 0);
    }
}
