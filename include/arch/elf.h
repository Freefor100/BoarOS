#ifndef BOAROS_ARCH_ELF_H
#define BOAROS_ARCH_ELF_H
#include <kernel/elf64.h>
#if defined(BOAROS_ARCH_LOONGARCH)
#define ARCH_ELF_MACHINE KERNEL_ELF64_MACHINE_LOONGARCH
uint64_t la_user_hwcap(void);
#define ARCH_ELF_HWCAP la_user_hwcap()
#else
#define ARCH_ELF_MACHINE KERNEL_ELF64_MACHINE_RISCV
#if defined(__riscv)
#include <arch/riscv/vector.h>
#define ARCH_ELF_AUX_EXTRA 1
#define ARCH_ELF_MINSIGSTKSZ (riscv_vector_vlenb ? 1152+32*riscv_vector_vlenb : 1088)
#define RISCV_VECTOR_HWCAP (riscv_vector_vlenb ? (UINT64_C(1)<<21) : 0)
#else
#define RISCV_VECTOR_HWCAP 0
#endif
#define ARCH_ELF_HWCAP (RISCV_VECTOR_HWCAP | (UINT64_C(1)<<0)|(UINT64_C(1)<<2)|(UINT64_C(1)<<3)|(UINT64_C(1)<<5)|(UINT64_C(1)<<8)|(UINT64_C(1)<<12))
#endif
#ifndef ARCH_ELF_AUX_EXTRA
#define ARCH_ELF_AUX_EXTRA 0
#endif
extern const unsigned char arch_elf_vdso_start[], arch_elf_vdso_end[];
#endif
