#ifndef BOAROS_ARCH_RISCV_PROCESS_H
#define BOAROS_ARCH_RISCV_PROCESS_H

#include <arch/riscv/trap.h>
#include <kernel/scheduler.h>

#include <stdint.h>

/* Clone the current process from the syscall-entry register snapshot. */
enum kernel_scheduler_status riscv_process_clone_current(
    const struct riscv_trap_frame *parent_frame,
    uint64_t child_stack,
    int64_t *linux_result);

#endif
