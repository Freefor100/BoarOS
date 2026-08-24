#ifndef BOAROS_ARCH_RISCV_THREAD_H
#define BOAROS_ARCH_RISCV_THREAD_H

#define RISCV_THREAD_STATE_KERNEL_SP 0
#define RISCV_THREAD_STATE_USER_SP 8
#define RISCV_THREAD_STATE_USER_MODE 16
#define RISCV_THREAD_STATE_SATP 24
#define RISCV_THREAD_STATE_SIZE 32

#ifndef __ASSEMBLER__

#include <stddef.h>
#include <stdint.h>

struct riscv_thread_state {
    uintptr_t kernel_sp;
    uintptr_t user_sp;
    uint64_t user_mode;
    uint64_t satp;
} __attribute__((aligned(16)));

_Static_assert(offsetof(struct riscv_thread_state, kernel_sp) ==
                   RISCV_THREAD_STATE_KERNEL_SP,
               "RISC-V thread kernel-sp offset mismatch");
_Static_assert(offsetof(struct riscv_thread_state, user_sp) ==
                   RISCV_THREAD_STATE_USER_SP,
               "RISC-V thread user-sp offset mismatch");
_Static_assert(offsetof(struct riscv_thread_state, user_mode) ==
                   RISCV_THREAD_STATE_USER_MODE,
               "RISC-V thread user-mode offset mismatch");
_Static_assert(offsetof(struct riscv_thread_state, satp) ==
                   RISCV_THREAD_STATE_SATP,
               "RISC-V thread satp offset mismatch");
_Static_assert(sizeof(struct riscv_thread_state) ==
                   RISCV_THREAD_STATE_SIZE,
               "RISC-V thread state size mismatch");

#endif

#endif
