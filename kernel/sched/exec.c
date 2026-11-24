#include <arch/task.h>
#include "../exec_internal.h"
#include "private.h"

#include <kernel/exec.h>
#include <kernel/credentials.h>
#include <kernel/ptrace.h>
#include <kernel/proc_task.h>
#include <kernel/futex.h>
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
    mm->record = 0;
    mm->record_page_address = 0U;
    mm->state = KERNEL_MM_MOVED;
    mm->cleanup_stage = KERNEL_MM_CLEANUP_NONE;
}

enum kernel_scheduler_status kernel_scheduler_exec_commit(void)
{
    struct kernel_task *thread;
    struct kernel_exec_transaction *transaction;
    struct kernel_mm_mapping entry_mapping;
    struct kernel_mm_mapping stack_mapping;
    struct kernel_vma entry_vma;
    uint64_t new_context;
    enum kernel_exec_status exec_status;
    enum kernel_files_status files_status;
    enum kernel_mm_status mm_status;
    enum kernel_scheduler_status status;
    kernel_pid_t old_tid;
    uint32_t old_trace_tid;
    int entry_present = 0;

    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED) {
        return KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (arch_interrupt_is_enabled()) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    status = validate_current();
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }
    thread = kernel_cpu_current()->current;
    if(kernel_task_user_tid(thread,&old_tid)!=KERNEL_TASK_STATUS_OK)__builtin_trap();
    transaction = thread->exec_transaction;
    if (thread == kernel_cpu_current()->idle || thread->arch.user_mode != 1U ||
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
    mm_status = kernel_mm_context(&transaction->image.mm, &new_context);
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
            !(entry_vma.permissions & KERNEL_MM_EXECUTE)) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
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

#if BOAROS_USER_SMP
    if (!kernel_scheduler_user_service_active())
        kernel_scheduler_enter_user_service();
#endif
    old_trace_tid = kernel_ptrace_exec_old_tid(thread);
    status = process_group_exec_current();
    if (status != KERNEL_SCHEDULER_STATUS_OK) return status;
    thread->fork_no_exec = 0U;
    /* The caller may have adopted the leader's TID. Release robust owners
     * under their old TID before any later allocation can fail. */
    kernel_futex_release_robust(thread, old_tid);
    enum kernel_signal_status signal_status=kernel_signal_unshare_for_exec(thread);
    if (signal_status != KERNEL_SIGNAL_STATUS_OK)
        kernel_user_thread_exit(KERNEL_THREAD_EXIT_USER_FAULT,UINT64_C(0x45584543),signal_status);
    /* MM cleanup also needs the exec actor when the FD table is private. */
    transaction->cleanup_task=thread;
    if (kernel_files_is_shared(&thread->files)) {
        /* A process outside this thread group may retain the old table. */
        files_status=kernel_files_fork(&transaction->prepared_files,&thread->files);
        if (files_status != KERNEL_FILES_STATUS_OK)
            kernel_user_thread_exit(KERNEL_THREAD_EXIT_USER_FAULT,UINT64_C(0x45584543),files_status);
        {
            KERNEL_RAW_SCOPE(access,&thread->files_access);
            transaction->retired_files=thread->files;
            thread->files=transaction->prepared_files;
            transaction->prepared_files=(struct kernel_files){0};
        }
    }
    /* Keep the old MM user registered through the allocations above so an
     * exec failure can remove that user through the normal exit path. */
    kernel_futex_release_mm(thread);
    /* 身份/MM 发布前撤下 CLOEXEC 槽；保留清理 owner 处理后端释放失败。 */
    files_status = kernel_files_close_on_exec(&thread->files);
    if (files_status != KERNEL_FILES_STATUS_OK &&
        files_status != KERNEL_FILES_STATUS_CLEANUP_REQUIRED)
        kernel_user_thread_exit(KERNEL_THREAD_EXIT_USER_FAULT,UINT64_C(0x45584543),files_status);
    if (kernel_mm_activate(&transaction->image.mm, scheduler.kernel_context) != KERNEL_MM_STATUS_OK) {
        kernel_user_thread_exit(KERNEL_THREAD_EXIT_USER_FAULT,UINT64_C(0x45584543),KERNEL_MM_STATUS_ADDRESS_SPACE);
    }
    scheduler_usage_save_mm(thread);
    kernel_mm_add_user(&transaction->image.mm);
    const struct kernel_credentials *previous;
    struct kernel_exec_security *previous_security;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        kernel_mm_coredump_filter_set(&transaction->image.mm,
            kernel_mm_coredump_filter_get(&thread->mm));
        kernel_mm_thp_disable_set(&transaction->image.mm,kernel_mm_thp_disable_get(&thread->mm));
        transaction->retired_mm = thread->mm;
        thread->mm = transaction->image.mm;
        previous=thread->credentials;
        thread->credentials=transaction->credentials;
        thread->personality=transaction->personality;
        transaction->credentials=0;
        previous_security=thread->exec_security;
        thread->exec_security=transaction->exec_security;
        transaction->exec_security=0;
        /* commit_creds applies to the new image. The old image may still
         * be shared by another process through CLONE_VM. */
        kernel_credentials_changed_locked(thread, previous, thread->credentials);
        if (transaction->secureexec) thread->parent_death_signal = 0;
        finish_mm_move(&transaction->image.mm);
    }
    kernel_credentials_put(previous);
    kernel_exec_security_put(previous_security);
    kernel_task_comm_set(thread, transaction->comm);
    arch_thread_set_mm(&thread->arch, new_context);
    arch_process_prepare_exec(thread,
                          transaction->image.entry,
                          transaction->image.stack_pointer,
                          transaction->image.thread_pointer);
    kernel_signal_reset_on_exec(thread);
    __atomic_store_n(&transaction->state,KERNEL_EXEC_TRANSACTION_COMMITTED_CLEANUP,__ATOMIC_RELEASE);
    (void)kernel_wait_queue_wake_all(&thread->child_exit_queue);

    exec_status = kernel_exec_transaction_cleanup(transaction);
    /* The parent needs the shared MM back, not completion of unrelated
     * executable or descriptor cleanup that may have to be retried. */
    if (exec_status == KERNEL_EXEC_STATUS_CLEANUP_REQUIRED &&
        transaction->retired_mm.state == KERNEL_MM_RELEASED)
        process_complete_vfork(thread);
    if (exec_status == KERNEL_EXEC_STATUS_OK) {
        (void)kernel_task_exec_finish(thread, transaction);
    } else if (exec_status != KERNEL_EXEC_STATUS_CLEANUP_REQUIRED) {
        kernel_user_thread_exit(KERNEL_THREAD_EXIT_USER_FAULT,
                                UINT64_C(0x45584543),
                                exec_status);
    }
    kernel_ptrace_exec(thread,old_trace_tid);
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
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        task->exec_transaction = transaction;
    }
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
    /* Also reached after a real source/OFD cleanup retry. No user of the
     * replaced MM remains when the transaction container is finally freed. */
    if (transaction->retired_mm.state == KERNEL_MM_RELEASED)
        process_complete_vfork(task);
    heap = transaction->heap;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        task->exec_transaction = 0;
    }
    kernel_heap_release(heap, transaction);
    return KERNEL_EXEC_STATUS_OK;
}

int kernel_task_exec_busy_locked(struct kernel_task *task)
{
    if(!kernel_raw_lock_owned(&kernel_wait_domain))__builtin_trap();
    if(task->proc_exiting || !task->exec_transaction)return 0;
    unsigned state=__atomic_load_n(&task->exec_transaction->state,__ATOMIC_ACQUIRE);
    return state==KERNEL_EXEC_TRANSACTION_PREPARING || state==KERNEL_EXEC_TRANSACTION_PREPARED;
}

void kernel_task_exec_abort(struct kernel_task *task,struct kernel_exec_transaction *transaction)
{
    int changed=0;
    {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        if(task->exec_transaction!=transaction)__builtin_trap();
        unsigned state=__atomic_load_n(&transaction->state,__ATOMIC_RELAXED);
        if(state==KERNEL_EXEC_TRANSACTION_PREPARING || state==KERNEL_EXEC_TRANSACTION_PREPARED) {
            __atomic_store_n(&transaction->state,KERNEL_EXEC_TRANSACTION_ABORTED_CLEANUP,__ATOMIC_RELEASE);
            changed=1;
        }
    }
    if(changed)(void)kernel_wait_queue_wake_all(&task->child_exit_queue);
}
