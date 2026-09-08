#ifndef BOAROS_ARCH_RISCV_SIGNAL_H
#define BOAROS_ARCH_RISCV_SIGNAL_H

struct riscv_trap_frame;

void riscv_signal_prepare_user_return(struct riscv_trap_frame *frame);
void riscv_signal_restore_current(struct riscv_trap_frame *frame);

#endif
