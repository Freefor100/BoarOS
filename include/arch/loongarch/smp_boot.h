#ifndef BOAROS_ARCH_LOONGARCH_SMP_BOOT_H
#define BOAROS_ARCH_LOONGARCH_SMP_BOOT_H
#define LA_SMP_BOOT_HARDWARE 0
#define LA_SMP_BOOT_THREAD 8
#define LA_SMP_BOOT_STACK_LOW 16
#define LA_SMP_BOOT_STACK_HIGH 24
#define LA_SMP_BOOT_SIZE 32
#define LA_SMP_STACK_SIZE 16384
#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdint.h>
struct kernel_task;
/* MBUF1 borrows this permanent descriptor; MBUF0 belongs to firmware entry. */
struct la_smp_boot {
    uint64_t hardware_id;
    struct kernel_task *thread;
    uintptr_t stack_low, stack_high;
};
_Static_assert(offsetof(struct la_smp_boot,hardware_id)==LA_SMP_BOOT_HARDWARE,"LA boot hardware prefix");
_Static_assert(offsetof(struct la_smp_boot,thread)==LA_SMP_BOOT_THREAD,"LA boot thread prefix");
_Static_assert(offsetof(struct la_smp_boot,stack_low)==LA_SMP_BOOT_STACK_LOW,"LA boot stack low prefix");
_Static_assert(offsetof(struct la_smp_boot,stack_high)==LA_SMP_BOOT_STACK_HIGH,"LA boot stack high prefix");
_Static_assert(sizeof(struct la_smp_boot)==LA_SMP_BOOT_SIZE,"LA boot descriptor stride");
#endif
#endif
