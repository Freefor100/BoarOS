#include <arch/riscv/trap.h>

void __real_riscv_trap_dispatch(struct riscv_trap_frame *frame);

void __wrap_riscv_trap_dispatch(struct riscv_trap_frame *frame)
{
    __real_riscv_trap_dispatch(frame);

    if ((frame->sstatus & RISCV_SSTATUS_SPP) == 0U &&
        frame->scause == RISCV_SCAUSE_USER_ECALL) {
        frame->kernel_tp = 0U;
    }
}
