#include <arch/riscv/process.h>
#include <arch/riscv/fpu.h>
#include <arch/riscv/thread.h>
#include <string.h>

#include "../../kernel/sched/private.h"

enum kernel_scheduler_status riscv_process_prepare_clone(
    struct kernel_task *child, struct kernel_task *parent,
    const struct riscv_trap_frame *parent_frame,
    uint64_t child_stack, int set_tls, uint64_t tls)
{
    struct riscv_trap_frame *frame = (struct riscv_trap_frame *)(
        child->stack_high - sizeof(*frame));

    if ((uintptr_t)frame < child->stack_low)
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    riscv_fpu_state_save(&parent->fpu);
    child->fpu = parent->fpu;
    *frame = *parent_frame;
    frame->sstatus = (frame->sstatus & ~RISCV_SSTATUS_FS_MASK) |
                     RISCV_SSTATUS_FS_CLEAN;
    frame->a0 = 0U;
    frame->sepc += 4U;
    frame->scause = 0U;
    frame->stval = 0U;
    frame->kernel_tp = (uintptr_t)child;
    if (child_stack != 0U) frame->sp = child_stack;
    if (set_tls) frame->tp = tls;
    return riscv_context_init_user(&child->context, (uintptr_t)frame, child) ==
                   RISCV_CONTEXT_STATUS_OK
               ? KERNEL_SCHEDULER_STATUS_OK
               : KERNEL_SCHEDULER_STATUS_INVALID_STATE;
}

void riscv_process_prepare_exec(struct kernel_task *task, uintptr_t entry,
                                uintptr_t stack, uintptr_t tls)
{
    struct riscv_trap_frame *frame = (struct riscv_trap_frame *)(
        task->stack_high - sizeof(*frame));

    memset(frame, 0, sizeof(*frame));
    frame->sp = stack;
    frame->tp = tls;
    frame->sstatus = RISCV_SSTATUS_SPIE | RISCV_SSTATUS_UXL_64 |
                     RISCV_SSTATUS_FS_INITIAL;
    frame->sepc = entry;
    frame->kernel_tp = (uintptr_t)task;
    riscv_fpu_reset_current(&task->fpu);
}
