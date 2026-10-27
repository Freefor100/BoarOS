#ifndef BOAROS_ARCH_SMP_H
#define BOAROS_ARCH_SMP_H
#if defined(BOAROS_ARCH_LOONGARCH)
#include <arch/loongarch/smp.h>
#define arch_smp_cpu la_smp_cpu
#define ARCH_IPI_DEADLINE LA_IPI_DEADLINE
#else
#include <arch/riscv/smp.h>
#define arch_smp_cpu riscv_smp_cpu
#define ARCH_IPI_DEADLINE RISCV_IPI_DEADLINE
#endif
#endif
