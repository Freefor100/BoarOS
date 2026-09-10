#include "../exec_internal.h"
#include "private.h"

#include <arch/riscv/fpu.h>
#include <arch/riscv/mm.h>
#include <arch/riscv/sv39.h>
#include <arch/riscv/thread.h>
#include <arch/riscv/trap.h>
#include <kernel/exec.h>
#include <kernel/files.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
#include <kernel/scheduler.h>
#include <kernel/signal.h>

#include <stddef.h>
#include <stdint.h>

static void finish_mm_move(struct kernel_mm *mm)
{
    mm->allocator = 0;
    mm->record_page_address = 0U;
    mm->state = KERNEL_MM_MOVED;
    mm->cleanup_stage = KERNEL_MM_CLEANUP_NONE;
}

static void reset_exec_trap_frame(struct kernel_task *thread,
                                  uintptr_t entry,
                                  uintptr_t stack_pointer,
                                  uintptr_t thread_pointer)
{
    struct riscv_trap_frame *frame =
        (struct riscv_trap_frame *)(thread->stack_high -
                                    sizeof(struct riscv_trap_frame));
    volatile unsigned char *bytes = (volatile unsigned char *)frame;
    size_t index;

    for (index = 0U; index < sizeof(*frame); index++) {
        bytes[index] = 0U;
    }
    frame->sp = stack_pointer;
    frame->tp = thread_pointer;
    frame->sstatus = RISCV_SSTATUS_SPIE | RISCV_SSTATUS_UXL_64 |
                     RISCV_SSTATUS_FS_INITIAL;
    frame->sepc = entry;
    frame->kernel_tp = (uintptr_t)thread;
    /* The execing task returns to user space without a context switch,
     * so the old image's register contents are cleared here rather than
     * by the first dispatch. */
    riscv_fpu_reset_current(&thread->fpu);
}

enum kernel_scheduler_status kernel_scheduler_exec_commit(void)
{
    struct kernel_task *thread;
    struct kernel_exec_transaction *transaction;
    struct kernel_mm_mapping entry_mapping;
    struct kernel_mm_mapping stack_mapping;
    struct kernel_vma entry_vma;
    uint64_t new_satp;
    enum kernel_exec_status exec_status;
    enum kernel_files_status files_status;
    enum kernel_mm_status mm_status;
    enum kernel_scheduler_status status;
    int entry_present = 0;

    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED) {
        return KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (riscv_interrupt_is_enabled()) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    status = validate_current();
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }
    thread = scheduler.current;
    transaction = thread->exec_transaction;
    if (thread == &scheduler.idle || thread->arch.user_mode != 1U ||
        thread->group_leader != thread || thread->group_members != 1U ||
        transaction == 0 ||
        transaction->state != KERNEL_EXEC_TRANSACTION_PREPARED ||
        transaction->retired_mm.state != KERNEL_MM_EMPTY ||
        transaction->image.mm.allocator != scheduler.allocator ||
        transaction->image.entry == 0U ||
        (transaction->image.entry & (uintptr_t)1U) != 0U ||
        transaction->image.stack_pointer == 0U ||
        (transaction->image.stack_pointer & (uintptr_t)15U) != 0U) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    mm_status = riscv_kernel_mm_satp(&transaction->image.mm, &new_satp);
    if (mm_status != KERNEL_MM_STATUS_OK) {
        return KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE;
    }
    mm_status = kernel_mm_lookup(&transaction->image.mm,
                                 transaction->image.entry,
                                 &entry_mapping);
    if (mm_status == KERNEL_MM_STATUS_OK) {
        entry_present = 1;
    }
    if (mm_status == KERNEL_MM_STATUS_NOT_MAPPED) {
        mm_status = kernel_mm_vma_lookup(&transaction->image.mm,
                                         transaction->image.entry,
                                         &entry_vma);
        if (mm_status == KERNEL_MM_STATUS_OK &&
            (entry_vma.permissions & KERNEL_MM_EXECUTE) != 0U) {
            mm_status = KERNEL_MM_STATUS_OK;
        }
    }
    if (mm_status != KERNEL_MM_STATUS_OK ||
        (entry_present != 0 &&
         (entry_mapping.permissions &
          (KERNEL_MM_USER | KERNEL_MM_EXECUTE)) !=
             (KERNEL_MM_USER | KERNEL_MM_EXECUTE))) {
        return mm_status == KERNEL_MM_STATUS_ADDRESS_SPACE ||
                       mm_status == KERNEL_MM_STATUS_PAGE_ACCESS
                   ? KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE
                   : KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    mm_status = kernel_mm_lookup(
        &transaction->image.mm,
        transaction->image.stack_pointer - 1U,
        &stack_mapping);
    if (mm_status != KERNEL_MM_STATUS_OK ||
        (stack_mapping.permissions &
         (KERNEL_MM_USER | KERNEL_MM_READ | KERNEL_MM_WRITE)) !=
            (KERNEL_MM_USER | KERNEL_MM_READ | KERNEL_MM_WRITE)) {
        return mm_status == KERNEL_MM_STATUS_ADDRESS_SPACE ||
                       mm_status == KERNEL_MM_STATUS_PAGE_ACCESS
                   ? KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE
                   : KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }

    if (riscv_sv39_switch_satp(new_satp) != RISCV_SV39_STATUS_OK) {
        return KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE;
    }
    transaction->retired_mm = thread->mm;
    thread->mm = transaction->image.mm;
    finish_mm_move(&transaction->image.mm);
    thread->arch.satp = new_satp;
    reset_exec_trap_frame(thread,
                          transaction->image.entry,
                          transaction->image.stack_pointer,
                          transaction->image.thread_pointer);
    kernel_signal_reset_on_exec(thread);
    transaction->state = KERNEL_EXEC_TRANSACTION_COMMITTED_CLEANUP;

    files_status = kernel_files_close_on_exec(&thread->files);
    if (files_status != KERNEL_FILES_STATUS_OK &&
        files_status != KERNEL_FILES_STATUS_CLEANUP_REQUIRED) {
        kernel_user_thread_exit(KERNEL_THREAD_EXIT_USER_FAULT,
                                UINT64_C(0x45584543),
                                files_status);
    }
    exec_status = kernel_exec_transaction_cleanup(transaction);
    if (exec_status == KERNEL_EXEC_STATUS_OK) {
        (void)kernel_task_exec_finish(thread, transaction);
    } else if (exec_status != KERNEL_EXEC_STATUS_CLEANUP_REQUIRED) {
        kernel_user_thread_exit(KERNEL_THREAD_EXIT_USER_FAULT,
                                UINT64_C(0x45584543),
                                exec_status);
    }
    return KERNEL_SCHEDULER_STATUS_OK;
}

enum kernel_exec_status kernel_task_exec_attach(
    struct kernel_task *task,
    struct kernel_exec_transaction *transaction)
{
    if (task == 0 || transaction == 0) {
        return KERNEL_EXEC_STATUS_INVALID_ARGUMENT;
    }
    if (validate_task_resource_borrow(task) != KERNEL_TASK_STATUS_OK ||
        task->exec_transaction != 0 ||
        !kernel_exec_transaction_valid(transaction, task->files.heap)) {
        return KERNEL_EXEC_STATUS_STATE;
    }
    task->exec_transaction = transaction;
    return KERNEL_EXEC_STATUS_OK;
}

enum kernel_exec_status kernel_task_exec_borrow(
    struct kernel_task *task,
    struct kernel_exec_transaction **transaction)
{
    if (task == 0 || transaction == 0) {
        return KERNEL_EXEC_STATUS_INVALID_ARGUMENT;
    }
    if (validate_task_resource_borrow(task) != KERNEL_TASK_STATUS_OK ||
        !kernel_exec_transaction_valid(task->exec_transaction,
                                       task->files.heap)) {
        return KERNEL_EXEC_STATUS_STATE;
    }
    *transaction = task->exec_transaction;
    return KERNEL_EXEC_STATUS_OK;
}

enum kernel_exec_status kernel_task_exec_finish(
    struct kernel_task *task,
    struct kernel_exec_transaction *transaction)
{
    struct kernel_heap *heap;

    if (task == 0 || transaction == 0) {
        return KERNEL_EXEC_STATUS_INVALID_ARGUMENT;
    }
    if (validate_task_resource_borrow(task) != KERNEL_TASK_STATUS_OK ||
        task->exec_transaction != transaction ||
        transaction->state != KERNEL_EXEC_TRANSACTION_EMPTY_CLEANUP ||
        !kernel_exec_transaction_valid(transaction, task->files.heap)) {
        return KERNEL_EXEC_STATUS_STATE;
    }
    /* Also reached by a later cleanup retry. No user of the retired MM
     * remains, even if freeing the transaction container needs retry. */
    process_complete_vfork(task);
    heap = transaction->heap;
    if (kernel_heap_release(heap, transaction) != KERNEL_HEAP_STATUS_OK) {
        return KERNEL_EXEC_STATUS_CLEANUP_REQUIRED;
    }
    task->exec_transaction = 0;
    return KERNEL_EXEC_STATUS_OK;
}
