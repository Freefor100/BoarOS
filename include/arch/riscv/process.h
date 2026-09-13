#ifndef BOAROS_ARCH_RISCV_PROCESS_H
#define BOAROS_ARCH_RISCV_PROCESS_H

#include <arch/riscv/trap.h>
#include <kernel/scheduler.h>

#include <stdint.h>

struct riscv_fpu_state;
struct kernel_task;
enum kernel_scheduler_status riscv_process_prepare_clone(
    struct kernel_task *child, struct kernel_task *parent,
    const struct riscv_trap_frame *parent_frame,
    uint64_t child_stack, int set_tls, uint64_t tls);
void riscv_process_prepare_exec(struct kernel_task *task, uintptr_t entry,
                                uintptr_t stack, uintptr_t tls);
/* Borrow the current user task's architecture-owned register image. */
struct riscv_fpu_state *riscv_process_fpu_borrow_current(void);

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
    uint64_t parent_tid,
    uint64_t tls,
    uint64_t child_tid,
    int64_t *linux_result);

#endif
