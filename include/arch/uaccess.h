#ifndef BOAROS_ARCH_UACCESS_H
#define BOAROS_ARCH_UACCESS_H
#if defined(__riscv)
#include <arch/riscv/uaccess.h>
#define ARCH_UACCESS_NATIVE 1
#define arch_uaccess_result riscv_uaccess_result
#define arch_copy_to_user riscv_copy_to_user
#define arch_copy_from_user riscv_copy_from_user
#define arch_copy_string_from_user riscv_copy_string_from_user
#else
/* LA and the host MM model use the real software mapping/loan protocol. */
#define ARCH_UACCESS_NATIVE 0
#endif
#endif
