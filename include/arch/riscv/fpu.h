#ifndef BOAROS_ARCH_RISCV_FPU_H
#define BOAROS_ARCH_RISCV_FPU_H

#define RISCV_FPU_STATE_REGS_OFFSET 0
#define RISCV_FPU_STATE_FCSR_OFFSET 256
#define RISCV_FPU_STATE_SAVED_OFFSET 264
#define RISCV_FPU_STATE_SIZE 272

#ifndef __ASSEMBLER__

#include <stddef.h>
#include <stdint.h>

/* Per-task floating-point state in the 64-bit D-extension register view.
 * `saved` means the memory image is authoritative and must be reloaded
 * into the registers before this state runs user code again; kernel
 * threads keep it zero and are never restored. */
struct riscv_fpu_state {
    uint64_t regs[32];
    uint64_t fcsr;
    uint64_t saved;
} __attribute__((aligned(16)));

_Static_assert(offsetof(struct riscv_fpu_state, regs) ==
                   RISCV_FPU_STATE_REGS_OFFSET,
               "RISC-V FPU state regs offset mismatch");
_Static_assert(offsetof(struct riscv_fpu_state, fcsr) ==
                   RISCV_FPU_STATE_FCSR_OFFSET,
               "RISC-V FPU state fcsr offset mismatch");
_Static_assert(offsetof(struct riscv_fpu_state, saved) ==
                   RISCV_FPU_STATE_SAVED_OFFSET,
               "RISC-V FPU state saved offset mismatch");
_Static_assert(sizeof(struct riscv_fpu_state) == RISCV_FPU_STATE_SIZE,
               "RISC-V FPU state size mismatch");

void riscv_fpu_switch(struct riscv_fpu_state *previous,
                      struct riscv_fpu_state *next);
void riscv_fpu_state_save(struct riscv_fpu_state *state);
void riscv_fpu_state_restore(struct riscv_fpu_state *state);
void riscv_fpu_reset_current(struct riscv_fpu_state *state);

#endif

#endif
