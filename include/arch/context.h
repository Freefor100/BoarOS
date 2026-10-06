#ifndef BOAROS_ARCH_CONTEXT_H
#define BOAROS_ARCH_CONTEXT_H
#if defined(BOAROS_ARCH_LOONGARCH)
#include <arch/loongarch/context.h>
#else
#include <arch/riscv/context.h>
#define arch_interrupt_save riscv_interrupt_save
#define arch_interrupt_restore riscv_interrupt_restore
#define arch_interrupt_is_enabled riscv_interrupt_is_enabled
#define arch_current_thread_get riscv_current_thread_get
#define arch_current_thread_set riscv_current_thread_set
#endif
#endif
