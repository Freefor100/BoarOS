#ifndef BOAROS_ARCH_RISCV_PROCESS_H
#define BOAROS_ARCH_RISCV_PROCESS_H

#include <arch/riscv/trap.h>
#include <kernel/scheduler.h>

#include <stdint.h>

/*
 * Clone the current process from the syscall-entry register snapshot.
 * `flags` selects fork or the vfork form (CLONE_VM|CLONE_VFORK), which
 * shares the parent address space and suspends the parent until the
 * child execs or exits.
 */
enum kernel_scheduler_status riscv_process_clone_current(
    const struct riscv_trap_frame *parent_frame,
    uint64_t flags,
    uint64_t child_stack,
    int64_t *linux_result);

#endif
