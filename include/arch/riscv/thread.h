#ifndef BOAROS_ARCH_RISCV_THREAD_H
#define BOAROS_ARCH_RISCV_THREAD_H

#define RISCV_THREAD_STATE_KERNEL_SP 0
#define RISCV_THREAD_STATE_SIZE 16

#ifndef __ASSEMBLER__

#include <stddef.h>
#include <stdint.h>

struct riscv_thread_state {
    uintptr_t kernel_sp;
} __attribute__((aligned(16)));

_Static_assert(offsetof(struct riscv_thread_state, kernel_sp) ==
                   RISCV_THREAD_STATE_KERNEL_SP,
               "RISC-V thread kernel-sp offset mismatch");
_Static_assert(sizeof(struct riscv_thread_state) ==
                   RISCV_THREAD_STATE_SIZE,
               "RISC-V thread state size mismatch");

#endif

#endif
