#ifndef BOAROS_ARCH_ELF_H
#define BOAROS_ARCH_ELF_H
#include <kernel/elf64.h>
#if defined(BOAROS_ARCH_LOONGARCH)
#define ARCH_ELF_MACHINE KERNEL_ELF64_MACHINE_LOONGARCH
#define ARCH_ELF_HWCAP UINT64_C(3)
#else
#define ARCH_ELF_MACHINE KERNEL_ELF64_MACHINE_RISCV
#define ARCH_ELF_HWCAP ((UINT64_C(1)<<0)|(UINT64_C(1)<<2)|(UINT64_C(1)<<3)|(UINT64_C(1)<<5)|(UINT64_C(1)<<8)|(UINT64_C(1)<<12))
#endif
extern const unsigned char arch_elf_trampoline_start[], arch_elf_trampoline_end[];
#endif
