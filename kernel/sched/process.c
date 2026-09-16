#include <arch/riscv/context.h>
#include <arch/riscv/mm.h>
#include <arch/riscv/process.h>
#include <arch/riscv/sv39.h>
#include <arch/riscv/thread.h>
#include <arch/riscv/trap.h>
#include <kernel/errno.h>
#include <kernel/exec.h>
#include <kernel/files.h>
#include <kernel/futex.h>
#include <kernel/fs_context.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
#include <kernel/tick.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/pid.h>
#include <kernel/scheduler.h>
#include <kernel/signal.h>
#include <kernel/task.h>
#include <kernel/uaccess.h>

#include <stddef.h>
#include <stdint.h>

#include "../exec_internal.h"
#include "private.h"

#define LINUX_WNOHANG UINT32_C(0x00000001)
#define LINUX_WUNTRACED UINT32_C(0x00000002)
#define LINUX_WCONTINUED UINT32_C(0x00000008)
#define LINUX_WAIT_CONTINUED UINT32_C(0xffff)
#define LINUX___WNOTHREAD UINT32_C(0x20000000)
#define LINUX___WALL UINT32_C(0x40000000)
#define LINUX___WCLONE UINT32_C(0x80000000)
#define LINUX_WAIT4_SUPPORTED_OPTIONS \
    (LINUX_WNOHANG | LINUX_WUNTRACED | LINUX_WCONTINUED | \
     LINUX___WNOTHREAD | LINUX___WALL | LINUX___WCLONE)
#define LINUX_CLONE_VM UINT64_C(0x100)
#define LINUX_CLONE_VFORK UINT64_C(0x4000)
#define LINUX_CLONE_THREAD UINT64_C(0x10000)
#define LINUX_CLONE_SETTLS UINT64_C(0x80000)
#define LINUX_CLONE_PARENT_SETTID UINT64_C(0x100000)
#define LINUX_CLONE_CHILD_CLEARTID UINT64_C(0x200000)
#define LINUX_CLONE_CHILD_SETTID UINT64_C(0x1000000)

void process_group_initialize(struct kernel_task *task)
{
    task->group_next = task;
    task->group_previous = task;
    kernel_wait_queue_init(&task->group_wait_queue);
}

static kernel_pid_t child_reaper_tid(struct kernel_task *leader,
                                     const struct kernel_task *departing)
{
    struct kernel_task *member = leader;
    do {
        if (member != departing && !member->terminate_requested &&
            member->state != KERNEL_THREAD_STATE_EXITED &&
            member->state != KERNEL_THREAD_STATE_ZOMBIE &&
            member->state != KERNEL_THREAD_STATE_GROUP_DEAD)
            return member->tid;
        member = member->group_next;
    } while (member != leader);
    return leader->tid;
}

static void request_thread_termination(struct kernel_task *task)
{
    if (task->state == KERNEL_THREAD_STATE_EXITED ||
        task->state == KERNEL_THREAD_STATE_ZOMBIE ||
        task->state == KERNEL_THREAD_STATE_GROUP_DEAD) return;
    task->terminate_requested = 1U;
    if (task->vfork_waiting && task->vfork_wait_child != 0) {
        /* Fatal cancellation severs both pointers before the parent's
         * task can disappear. The child retains its independent MM ref. */
        task->vfork_wait_child->vfork_parent = 0;
        task->vfork_wait_child->vfork_child = 0U;
        task->vfork_wait_child = 0;
        task->vfork_waiting = 0U;
        if (task->state == KERNEL_THREAD_STATE_BLOCKED &&
            task->wait_queue == &task->vfork_done_queue) {
            blocked_unlink(task);
            scheduler_wake_task(task, KERNEL_WAIT_SIGNALLED);
        }
    }
    if (task->state == KERNEL_THREAD_STATE_STOPPED) {
        stopped_unlink(task);
        task->state = KERNEL_THREAD_STATE_READY;
        ready_append(task);
    }
    (void)kernel_scheduler_wake_signal(task);
}

void process_group_request_exit(struct kernel_task *task,
                                enum kernel_thread_exit_reason reason,
                                uint64_t status, uint64_t detail)
{
    struct kernel_task *leader = task->group_leader;
    struct kernel_task *member = leader;
    if (!leader->group_exiting) {
        leader->group_exiting = 1U;
        leader->completion.reason = reason;
        leader->completion.status = status;
        leader->completion.detail = detail;
    }
    do {
        request_thread_termination(member);
        member = member->group_next;
    } while (member != leader);
    (void)kernel_wait_queue_wake_all(&leader->group_wait_queue);
}

void kernel_user_group_exit(enum kernel_thread_exit_reason reason,
                            uint64_t status, uint64_t detail)
{
    process_group_request_exit(scheduler.current, reason, status, detail);
    kernel_user_thread_exit(reason, status, detail);
}

struct riscv_fpu_state *riscv_process_fpu_borrow_current(void)
{
    return &scheduler.current->fpu;
}

enum kernel_mm_status kernel_scheduler_resolve_current_user_fault(
    uint64_t virtual_address,
    uint32_t access)
{
    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED ||
        riscv_interrupt_is_enabled() ||
        validate_current() != KERNEL_SCHEDULER_STATUS_OK ||
        scheduler.current == &scheduler.idle ||
        scheduler.current->state != KERNEL_THREAD_STATE_RUNNING ||
        scheduler.current->arch.user_mode != 1U) {
        return KERNEL_MM_STATUS_STATE;
    }
    return kernel_mm_resolve_user_fault(&scheduler.current->mm,
                                        virtual_address,
                                        access);
}

static enum kernel_scheduler_status validate_child_list(
    const struct kernel_task *parent,
    const struct kernel_task *required_child)
{
    const struct kernel_task *previous = 0;
    const struct kernel_task *child;
    uint32_t count = 0U;
    int found = required_child == 0;

    if (parent == 0 || parent->magic != KERNEL_THREAD_MAGIC ||
        parent->arch.user_mode != 1U || parent->tid_owned != 1U ||
        parent->tid <= 0 ||
        ((parent->first_child == 0) != (parent->last_child == 0)) ||
        (parent->first_child != 0 &&
         parent->first_child->previous_sibling != 0) ||
        (parent->last_child != 0 &&
         parent->last_child->next_sibling != 0)) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    for (child = parent->first_child;
         child != 0;
         child = child->next_sibling) {
        if (++count > KERNEL_PID_LIMIT || child == parent ||
            child->magic != KERNEL_THREAD_MAGIC || child->idle != 0U ||
            child->arch.user_mode != 1U || child->tid_owned != 1U ||
            child->tid <= 0 || child->parent != parent ||
            child->previous_sibling != previous ||
            child->publish_completion != 0U ||
            child->state < KERNEL_THREAD_STATE_READY ||
            child->state > KERNEL_THREAD_STATE_GROUP_DEAD) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
        if (child == required_child) {
            found = 1;
        }
        previous = child;
    }
    if (previous != parent->last_child || !found) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    return KERNEL_SCHEDULER_STATUS_OK;
}

static enum kernel_scheduler_status validate_child_endpoints(
    const struct kernel_task *parent)
{
    if (parent == 0 || parent->magic != KERNEL_THREAD_MAGIC ||
        parent->arch.user_mode != 1U || parent->tid_owned != 1U ||
        parent->tid <= 0 ||
        ((parent->first_child == 0) != (parent->last_child == 0)) ||
        (parent->first_child != 0 &&
         parent->first_child->previous_sibling != 0) ||
        (parent->last_child != 0 &&
         (parent->last_child->next_sibling != 0 ||
          parent->last_child->parent != parent))) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    return KERNEL_SCHEDULER_STATUS_OK;
}

static void child_append(struct kernel_task *parent,
                         struct kernel_task *child)
{
    child->parent = parent;
    child->previous_sibling = parent->last_child;
    child->next_sibling = 0;
    if (parent->last_child == 0) {
        parent->first_child = child;
    } else {
        parent->last_child->next_sibling = child;
    }
    parent->last_child = child;
}

static void child_remove(struct kernel_task *parent,
                         struct kernel_task *child)
{
    if (child->previous_sibling == 0) {
        parent->first_child = child->next_sibling;
    } else {
        child->previous_sibling->next_sibling = child->next_sibling;
    }
    if (child->next_sibling == 0) {
        parent->last_child = child->previous_sibling;
    } else {
        child->next_sibling->previous_sibling =
            child->previous_sibling;
    }
    child->parent = 0;
    child->previous_sibling = 0;
    child->next_sibling = 0;
}

void wake_waiting_parent(struct kernel_task *child)
{
    struct kernel_task *parent = child->parent;

    if (parent != 0) {
        (void)kernel_wait_queue_wake_all(&parent->child_exit_queue);
    }
}

/* Notifies a vfork-suspended parent that the child released its shared
 * address space (through exec or exit). */
void process_complete_vfork(struct kernel_task *thread)
{
    if (thread->vfork_child != 0U && thread->vfork_parent != 0) {
        struct kernel_task *parent = thread->vfork_parent;
        /* Failed exec preparation also frees its transaction, but leaves
         * the child in the shared MM. That is not vfork completion. */
        if (thread->mm.state == KERNEL_MM_LIVE &&
            thread->mm.record_page_address ==
                parent->mm.record_page_address) {
            return;
        }
        /* Consume this child's completion before waking; an exec followed
         * by exit must not complete the parent's next vfork. */
        thread->vfork_child = 0U;
        thread->vfork_parent = 0;
        parent->vfork_waiting = 0U;
        parent->vfork_wait_child = 0;
        (void)kernel_wait_queue_wake_one(
            &parent->vfork_done_queue);
    }
}

static void exited_append(struct kernel_task *thread)
{
    thread->next = 0;
    if (scheduler.exited_tail == 0) {
        scheduler.exited_head = thread;
    } else {
        scheduler.exited_tail->next = thread;
    }
    scheduler.exited_tail = thread;
}

enum kernel_scheduler_status process_group_exec_current(void)
{
    struct kernel_task *task = scheduler.current;
    struct kernel_task *leader = task->group_leader;
    struct kernel_task *member;
    enum kernel_wait_wake_reason reason;
    enum kernel_scheduler_status status;

    /* The first prepared exec owns group teardown. A competing exec must
     * unwind its own prepared image through the ordinary exit cleanup. */
    if (leader->group_exiting || leader->group_execing ||
        task->terminate_requested)
        kernel_user_thread_exit(KERNEL_THREAD_EXIT_SYSCALL, 0U, 0U);
    leader->group_execing = 1U;
    member = leader;
    do {
        if (member != task) request_thread_termination(member);
        member = member->group_next;
    } while (member != leader);
    while (leader->group_members != (task == leader ? 1U : 2U) ||
           (task != leader && leader->state != KERNEL_THREAD_STATE_GROUP_DEAD)) {
        status = kernel_scheduler_block_current(&leader->group_wait_queue,
                                                 0U, 0, &reason);
        if (status != KERNEL_SCHEDULER_STATUS_OK) return status;
        if (leader->group_exiting)
            kernel_user_thread_exit(KERNEL_THREAD_EXIT_SYSCALL, 0U, 0U);
    }
    if (task != leader) {
        kernel_pid_t old_tid = task->tid;
        if (kernel_pid_release(&scheduler.pid_allocator, task->tid) !=
            KERNEL_PID_STATUS_OK) return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        task->tid = leader->tid;
        task->parent = leader->parent;
        task->previous_sibling = leader->previous_sibling;
        task->next_sibling = leader->next_sibling;
        if (task->parent != 0) {
            if (task->previous_sibling != 0)
                task->previous_sibling->next_sibling = task;
            else task->parent->first_child = task;
            if (task->next_sibling != 0)
                task->next_sibling->previous_sibling = task;
            else task->parent->last_child = task;
        }
        task->first_child = leader->first_child;
        task->last_child = leader->last_child;
        for (member = task->first_child; member != 0;
             member = member->next_sibling) {
            member->parent = task;
            if (member->child_creator_tid == old_tid)
                member->child_creator_tid = task->tid;
        }
        task->child_creator_tid = leader->child_creator_tid;
        task->user_ticks += leader->user_ticks;
        task->kernel_ticks += leader->kernel_ticks;
        task->child_user_ticks = leader->child_user_ticks;
        task->child_kernel_ticks = leader->child_kernel_ticks;
        task->group_pending = leader->group_pending;
        for (unsigned i = 0U; i < KERNEL_SIGNAL_COUNT; i++)
            task->group_sender[i] = leader->group_sender[i];
        task->publish_completion = leader->publish_completion;
        if (scheduler.init_task == leader) scheduler.init_task = task;
        leader->parent = 0;
        leader->first_child = leader->last_child = 0;
        leader->previous_sibling = leader->next_sibling = 0;
        leader->tid_owned = 0U;
        leader->tid = 0;
        leader->publish_completion = 0U;
        leader->group_leader = 0;
        leader->group_members = 0U;
        leader->state = KERNEL_THREAD_STATE_EXITED;
        exited_append(leader);
        task->group_leader = task;
        task->group_members = 1U;
        process_group_initialize(task);
    }
    task->group_execing = 0U;
    task->clear_tid_address = 0U;
    return KERNEL_SCHEDULER_STATUS_OK;
}

static int release_clone_resources(struct kernel_task *thread)
{
    int cleanup_failed = 0;

    if (kernel_signal_release_table(thread) !=
        KERNEL_SIGNAL_STATUS_OK) {
        cleanup_failed = 1;
    }

    if (thread->files.state == KERNEL_FILES_LIVE ||
        thread->files.state == KERNEL_FILES_CLEANUP) {
        if (kernel_files_release(&thread->files) !=
            KERNEL_FILES_STATUS_OK) {
            cleanup_failed = 1;
        }
    }
    if (thread->fs.state == KERNEL_FS_CONTEXT_LIVE ||
        thread->fs.state == KERNEL_FS_CONTEXT_CLEANUP) {
        if (kernel_fs_context_release(&thread->fs) !=
            KERNEL_FS_CONTEXT_STATUS_OK) {
            cleanup_failed = 1;
        }
    }
    if (thread->mm.state == KERNEL_MM_LIVE ||
        thread->mm.state == KERNEL_MM_CLEANUP) {
        if (kernel_mm_release(&thread->mm) != KERNEL_MM_STATUS_OK) {
            cleanup_failed = 1;
        }
    }
    return cleanup_failed;
}

static void queue_abandoned_clone(struct kernel_task *thread)
{
    thread->completion.kind = KERNEL_THREAD_KIND_USER;
    thread->completion.reason = KERNEL_THREAD_EXIT_USER_FAULT;
    thread->completion.tid = thread->tid;
    thread->completion.tgid = thread->tid;
    thread->completion.status = 0U;
    thread->completion.detail = 0U;
    thread->publish_completion = 0U;
    thread->state = KERNEL_THREAD_STATE_EXITED;
    exited_append(thread);
}

static enum kernel_scheduler_status finish_clone_failure(
    struct kernel_task *thread,
    int64_t linux_failure,
    int64_t *linux_result,
    enum kernel_scheduler_status scheduler_failure)
{
    int cleanup_failed;

    /* No failed construction has published a parent completion channel. */
    thread->vfork_parent = 0;
    thread->vfork_child = 0U;
    cleanup_failed = release_clone_resources(thread);

    if (thread->tid_owned != 0U) {
        if (kernel_pid_release(&scheduler.pid_allocator, thread->tid) !=
            KERNEL_PID_STATUS_OK) {
            cleanup_failed = 1;
        } else {
            thread->tid = 0;
            thread->process_group = 0;
            thread->tid_owned = 0U;
            thread->group_leader = 0;
            thread->group_members = 0U;
        }
    }
    if (!cleanup_failed) {
        (void)physical_page_release(scheduler.allocator,
                                  thread->physical_address);
    }
    if (cleanup_failed) {
        queue_abandoned_clone(thread);
    }
    *linux_result = linux_failure;
    return scheduler_failure;
}

enum kernel_scheduler_status riscv_process_clone_current(
    const struct riscv_trap_frame *parent_frame,
    uint64_t flags,
    uint64_t child_stack,
    uint64_t parent_tid,
    uint64_t tls,
    uint64_t child_tid,
    int64_t *linux_result)
{
    struct kernel_task *parent;
    struct kernel_task *child;
    uint64_t physical_address;
    uint64_t child_satp;
    uintptr_t stack_low;
    void *page;
    kernel_pid_t tid;
    uint32_t vfork = (flags & (LINUX_CLONE_VM | LINUX_CLONE_VFORK)) ==
                     (LINUX_CLONE_VM | LINUX_CLONE_VFORK);
    int thread_clone = (flags & LINUX_CLONE_THREAD) != 0U;
    enum kernel_wait_wake_reason wake_reason = KERNEL_WAIT_WOKEN;
    enum physical_page_status page_status;
    enum kernel_mm_status mm_status;
    enum kernel_files_status files_status;
    enum kernel_fs_context_status fs_status;
    enum kernel_pid_status pid_status;
    enum kernel_signal_status signal_status;
    enum kernel_scheduler_status status;

    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED) {
        return KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (parent_frame == 0 || linux_result == 0 ||
        riscv_interrupt_is_enabled()) {
        return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    }
    status = validate_current();
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }
    status = validate_queues();
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }
    parent = scheduler.current;
    if (parent == &scheduler.idle || parent->arch.user_mode != 1U ||
        parent->tid_owned != 1U ||
        parent_frame != (const struct riscv_trap_frame *)(
                            parent->stack_high - sizeof(*parent_frame)) ||
        parent_frame->kernel_tp != (uintptr_t)parent ||
        parent_frame->sepc > UINT64_MAX - 4U) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    status = validate_child_endpoints(parent);
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }

    page_status = physical_page_allocate(scheduler.allocator,
                                         &physical_address);
    if (page_status == PHYSICAL_PAGE_STATUS_EMPTY) {
        *linux_result = -KERNEL_ENOMEM;
        return KERNEL_SCHEDULER_STATUS_OK;
    }
    if (page_status != PHYSICAL_PAGE_STATUS_OK ||
        physical_page_resolve(scheduler.allocator,
                              physical_address,
                              &page) != PHYSICAL_PAGE_STATUS_OK) {
        if (page_status == PHYSICAL_PAGE_STATUS_OK) {
            (void)release_after_create_failure(
                physical_address,
                KERNEL_SCHEDULER_STATUS_PAGE_ACCESS);
        }
        return KERNEL_SCHEDULER_STATUS_PAGE_ACCESS;
    }

    clear_page(page);
    child = page;
    child->physical_address = physical_address;
    child->magic = KERNEL_THREAD_MAGIC;
    child->arch.user_mode = 1U;
    child->arch.kernel_sp = (uintptr_t)child + BOAROS_PAGE_SIZE;
    child->stack_high = (uintptr_t)child + BOAROS_PAGE_SIZE;
    stack_low = align_up_16((uintptr_t)child + sizeof(*child) +
                            sizeof(uint64_t));
    child->stack_low = stack_low;
    child->state = KERNEL_THREAD_STATE_EXITED;
    child->completion.kind = KERNEL_THREAD_KIND_USER;
    child->completion.reason = KERNEL_THREAD_EXIT_USER_FAULT;
    *(uint64_t *)(stack_low - sizeof(uint64_t)) = KERNEL_STACK_CANARY;

    /* vfork shares the parent address space instead of cloning it; the
     * shared record reference keeps the parent's handle valid across the
     * child's exec or exit. */
    if (vfork || thread_clone) {
        mm_status = kernel_mm_acquire(&child->mm, &parent->mm);
    } else {
        mm_status = kernel_mm_fork(&child->mm, &parent->mm);
    }
    if (mm_status != KERNEL_MM_STATUS_OK) {
        return finish_clone_failure(
            child,
            mm_status == KERNEL_MM_STATUS_NO_MEMORY
                ? -KERNEL_ENOMEM : -KERNEL_EAGAIN,
            linux_result,
            mm_status == KERNEL_MM_STATUS_NO_MEMORY ||
                    mm_status == KERNEL_MM_STATUS_CLEANUP_REQUIRED
                ? KERNEL_SCHEDULER_STATUS_OK
                : (mm_status == KERNEL_MM_STATUS_PAGE_ACCESS
                       ? KERNEL_SCHEDULER_STATUS_PAGE_ACCESS
                       : (mm_status == KERNEL_MM_STATUS_ADDRESS_SPACE
                              ? KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE
                              : KERNEL_SCHEDULER_STATUS_INVALID_STATE)));
    }
    if (parent->files.state != KERNEL_FILES_EMPTY) {
        files_status = thread_clone
            ? kernel_files_acquire(&child->files, &parent->files)
            : kernel_files_fork(&child->files, &parent->files);
        if (files_status != KERNEL_FILES_STATUS_OK) {
            return finish_clone_failure(
                child,
                files_status == KERNEL_FILES_STATUS_NO_MEMORY
                    ? -KERNEL_ENOMEM : -KERNEL_EAGAIN,
                linux_result,
                files_status == KERNEL_FILES_STATUS_NO_MEMORY ||
                        files_status ==
                            KERNEL_FILES_STATUS_CLEANUP_REQUIRED
                    ? KERNEL_SCHEDULER_STATUS_OK
                    : KERNEL_SCHEDULER_STATUS_INVALID_STATE);
        }
        fs_status = thread_clone
            ? kernel_fs_context_acquire(&child->fs, &parent->fs)
            : kernel_fs_context_fork(&child->fs, &parent->fs);
        if (fs_status != KERNEL_FS_CONTEXT_STATUS_OK) {
            return finish_clone_failure(
                child,
                fs_status == KERNEL_FS_CONTEXT_STATUS_NO_MEMORY
                    ? -KERNEL_ENOMEM : -KERNEL_EAGAIN,
                linux_result,
                fs_status == KERNEL_FS_CONTEXT_STATUS_NO_MEMORY ||
                        fs_status ==
                            KERNEL_FS_CONTEXT_STATUS_CLEANUP_REQUIRED
                    ? KERNEL_SCHEDULER_STATUS_OK
                    : KERNEL_SCHEDULER_STATUS_INVALID_STATE);
        }
    }
    pid_status = kernel_pid_allocate(&scheduler.pid_allocator, &tid);
    if (pid_status != KERNEL_PID_STATUS_OK) {
        return finish_clone_failure(
            child,
            pid_status == KERNEL_PID_STATUS_EXHAUSTED
                ? -KERNEL_EAGAIN : -KERNEL_ENOMEM,
            linux_result,
            pid_status == KERNEL_PID_STATUS_EXHAUSTED
                ? KERNEL_SCHEDULER_STATUS_OK
                : KERNEL_SCHEDULER_STATUS_INVALID_STATE);
    }
    child->tid = tid;
    child->process_group = parent->process_group;
    child->tid_owned = 1U;
    child->group_leader = child;
    child->group_members = 1U;
    process_group_initialize(child);
    child->publish_completion = 0U;
    child->vfork_child = vfork;
    child->vfork_parent = vfork ? parent : 0;
    child->completion.tid = tid;
    child->completion.tgid = tid;
    kernel_wait_queue_init(&child->child_exit_queue);
    kernel_wait_queue_init(&child->vfork_done_queue);
    signal_status = thread_clone ? kernel_signal_share(child, parent)
                                 : kernel_signal_fork(child, parent);
    if (signal_status != KERNEL_SIGNAL_STATUS_OK) {
        return finish_clone_failure(
            child,
            signal_status == KERNEL_SIGNAL_STATUS_NO_MEMORY
                ? -KERNEL_ENOMEM : -KERNEL_EAGAIN,
            linux_result,
            signal_status == KERNEL_SIGNAL_STATUS_NO_MEMORY
                ? KERNEL_SCHEDULER_STATUS_OK
                : KERNEL_SCHEDULER_STATUS_INVALID_STATE);
    }
    mm_status = riscv_kernel_mm_satp(&child->mm, &child_satp);
    if (mm_status != KERNEL_MM_STATUS_OK) {
        return finish_clone_failure(
            child, -KERNEL_EAGAIN, linux_result,
            KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE);
    }
    child->arch.satp = child_satp;
    if ((flags & LINUX_CLONE_CHILD_CLEARTID) != 0U)
        child->clear_tid_address = child_tid;
    status = riscv_process_prepare_clone(child, parent, parent_frame,
                                         child_stack,
                                         (flags & LINUX_CLONE_SETTLS) != 0U,
                                         tls);
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return finish_clone_failure(
            child, -KERNEL_EAGAIN, linux_result,
            KERNEL_SCHEDULER_STATUS_INVALID_STATE);
    }

    /* Linux does not roll clone back when a SETTID user store faults. */
    {
        size_t copied;
        if ((flags & LINUX_CLONE_PARENT_SETTID) != 0U)
            (void)kernel_copy_to_user(&parent->mm, parent_tid,
                                      &tid, sizeof(tid), &copied);
        if ((flags & LINUX_CLONE_CHILD_SETTID) != 0U)
            (void)kernel_copy_to_user(&child->mm, child_tid,
                                      &tid, sizeof(tid), &copied);
    }
    if (thread_clone) {
        struct kernel_task *leader = parent->group_leader;
        child->group_leader = leader;
        child->group_members = 0U;
        child->group_next = leader;
        child->group_previous = leader->group_previous;
        leader->group_previous->group_next = child;
        leader->group_previous = child;
        leader->group_members++;
        child->completion.tgid = leader->tid;
    } else {
        child->child_creator_tid = parent->tid;
        child_append(parent->group_leader, child);
    }
    child->state = KERNEL_THREAD_STATE_READY;
    ready_append(child);
    *linux_result = tid;

    if (vfork) {
        /* Linux suspends the vfork parent inside the syscall until the
         * child execs or exits; the child wakes vfork_done_queue. */
        kernel_wait_queue_init(&parent->vfork_done_queue);
        parent->vfork_waiting = 1U;
        parent->vfork_wait_child = child;
        while (parent->vfork_waiting != 0U) {
            status = kernel_scheduler_block_current(&parent->vfork_done_queue,
                                                    0U, 0, &wake_reason);
            if (status != KERNEL_SCHEDULER_STATUS_OK) {
                return status;
            }
        }
    }
    return KERNEL_SCHEDULER_STATUS_OK;
}

static int wait_child_matches(const struct kernel_task *parent,
                              const struct kernel_task *child,
                              int64_t pid)
{
    if (pid > 0) {
        return child->tid == pid;
    }
    if (pid == 0) {
        return child->process_group == parent->process_group;
    }
    if (pid == -1) {
        return 1;
    }
    return child->process_group == -pid;
}

static enum kernel_scheduler_status reap_waited_child(
    struct kernel_task *parent,
    struct kernel_task *child,
    uint64_t status_address,
    uint64_t rusage_address,
    int64_t *linux_result)
{
    kernel_pid_t pid = child->tid;
    uint32_t wait_status = child->wait_status;
    uint64_t user_ticks = child->user_ticks + child->child_user_ticks;
    uint64_t kernel_ticks = child->kernel_ticks + child->child_kernel_ticks;
    size_t copied = 0U;
    enum kernel_uaccess_status access_status = KERNEL_UACCESS_STATUS_OK;

    if (child->state != KERNEL_THREAD_STATE_ZOMBIE ||
        child->parent != parent->group_leader || child->tid_owned != 1U || pid <= 0 ||
        child->mm.state != KERNEL_MM_RELEASED ||
        (child->files.state != KERNEL_FILES_EMPTY &&
         child->files.state != KERNEL_FILES_RELEASED) ||
        (child->fs.state != KERNEL_FS_CONTEXT_EMPTY &&
         child->fs.state != KERNEL_FS_CONTEXT_RELEASED) ||
        child->exec_transaction != 0) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    if (kernel_pid_release(&scheduler.pid_allocator, pid) !=
        KERNEL_PID_STATUS_OK) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    parent->group_leader->child_user_ticks += user_ticks;
    parent->group_leader->child_kernel_ticks += kernel_ticks;
    child_remove(parent->group_leader, child);
    if (scheduler.init_task == child) {
        scheduler.init_task = 0;
    }
    child->tid = 0;
    child->process_group = 0;
    child->tid_owned = 0U;
    child->group_leader = 0;
    child->group_members = 0U;
    child->publish_completion = 0U;
    (void)physical_page_release(scheduler.allocator,
                              child->physical_address);

    if (status_address != 0U) {
        access_status = kernel_copy_to_user(&parent->mm,
                                            status_address,
                                            &wait_status,
                                            sizeof(wait_status),
                                            &copied);
    }
    if (access_status == KERNEL_UACCESS_STATUS_FAULT) {
        *linux_result = -KERNEL_EFAULT;
        return KERNEL_SCHEDULER_STATUS_OK;
    }
    if (access_status != KERNEL_UACCESS_STATUS_OK ||
        (status_address != 0U && copied != sizeof(wait_status))) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    if (rusage_address != 0U) {
        struct kernel_linux_rusage rusage = {0};
        size_t rusage_copied = 0U;
        rusage.ru_utime.tv_sec = (int64_t)(user_ticks /
                                           KERNEL_TICKS_PER_SECOND);
        rusage.ru_utime.tv_usec =
            (int64_t)(user_ticks % KERNEL_TICKS_PER_SECOND) *
            (int64_t)(1000000U / KERNEL_TICKS_PER_SECOND);
        rusage.ru_stime.tv_sec = (int64_t)(kernel_ticks /
                                           KERNEL_TICKS_PER_SECOND);
        rusage.ru_stime.tv_usec =
            (int64_t)(kernel_ticks % KERNEL_TICKS_PER_SECOND) *
            (int64_t)(1000000U / KERNEL_TICKS_PER_SECOND);
        access_status = kernel_copy_to_user(&parent->mm,
                                            rusage_address,
                                            &rusage,
                                            sizeof(rusage),
                                            &rusage_copied);
        if (access_status != KERNEL_UACCESS_STATUS_OK ||
            rusage_copied != sizeof(rusage)) {
            *linux_result = -KERNEL_EFAULT;
            return KERNEL_SCHEDULER_STATUS_OK;
        }
    }
    *linux_result = pid;
    return KERNEL_SCHEDULER_STATUS_OK;
}

static enum kernel_scheduler_status report_wait_event(
    struct kernel_task *parent,
    struct kernel_task *child,
    uint32_t wait_status,
    uint64_t status_address,
    uint64_t rusage_address,
    int64_t *linux_result)
{
    size_t copied = 0U;
    enum kernel_uaccess_status access_status;

    if (status_address != 0U) {
        access_status = kernel_copy_to_user(&parent->mm,
                                            status_address,
                                            &wait_status,
                                            sizeof(wait_status),
                                            &copied);
        if (access_status == KERNEL_UACCESS_STATUS_FAULT) {
            *linux_result = -KERNEL_EFAULT;
            return KERNEL_SCHEDULER_STATUS_OK;
        }
        if (access_status != KERNEL_UACCESS_STATUS_OK ||
            copied != sizeof(wait_status)) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
    }
    if (rusage_address != 0U) {
        struct kernel_linux_rusage rusage = {0};

        copied = 0U;
        access_status = kernel_copy_to_user(&parent->mm,
                                            rusage_address,
                                            &rusage,
                                            sizeof(rusage),
                                            &copied);
        if (access_status == KERNEL_UACCESS_STATUS_FAULT) {
            *linux_result = -KERNEL_EFAULT;
            return KERNEL_SCHEDULER_STATUS_OK;
        }
        if (access_status != KERNEL_UACCESS_STATUS_OK ||
            copied != sizeof(rusage)) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
    }
    *linux_result = child->tid;
    return KERNEL_SCHEDULER_STATUS_OK;
}

enum kernel_scheduler_status kernel_scheduler_wait4_current(
    int64_t pid,
    uint64_t status_address,
    uint32_t options,
    uint64_t rusage_address,
    int64_t *linux_result)
{
    struct kernel_task *parent;
    enum kernel_wait_wake_reason wake_reason = KERNEL_WAIT_WOKEN;
    enum kernel_scheduler_status status;

    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED) {
        return KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (linux_result == 0 || riscv_interrupt_is_enabled()) {
        return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    }
    if ((options & ~LINUX_WAIT4_SUPPORTED_OPTIONS) != 0U) {
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_SCHEDULER_STATUS_OK;
    }
    if (pid == INT32_MIN) {
        *linux_result = -KERNEL_ESRCH;
        return KERNEL_SCHEDULER_STATUS_OK;
    }

    for (;;) {
        struct kernel_task *child;
        struct kernel_task *previous = 0;
        uint32_t child_count = 0U;
        int matching_child = 0;

        status = validate_current();
        if (status != KERNEL_SCHEDULER_STATUS_OK) {
            return status;
        }
        status = validate_queues();
        if (status != KERNEL_SCHEDULER_STATUS_OK) {
            return status;
        }
        parent = scheduler.current;
        if (parent == &scheduler.idle || parent->arch.user_mode != 1U ||
            parent->tid_owned != 1U) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
        status = validate_child_endpoints(parent->group_leader);
        if (status != KERNEL_SCHEDULER_STATUS_OK) {
            return status;
        }
        for (child = parent->group_leader->first_child;
             child != 0;
             child = child->next_sibling) {
            if (++child_count > KERNEL_PID_LIMIT || child == parent ||
                child->magic != KERNEL_THREAD_MAGIC ||
                child->idle != 0U || child->arch.user_mode != 1U ||
                child->tid_owned != 1U || child->tid <= 0 ||
                child->parent != parent->group_leader ||
                child->previous_sibling != previous ||
                child->publish_completion != 0U ||
                child->state < KERNEL_THREAD_STATE_READY ||
                child->state > KERNEL_THREAD_STATE_GROUP_DEAD) {
                return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
            }
            previous = child;
            if ((options & LINUX___WNOTHREAD) != 0U &&
                child->child_creator_tid != parent->tid) continue;
            if ((options & LINUX___WCLONE) != 0U &&
                (options & LINUX___WALL) == 0U) {
                continue;
            }
            if (!wait_child_matches(parent, child, pid)) {
                continue;
            }
            matching_child = 1;
            if (child->state == KERNEL_THREAD_STATE_ZOMBIE) {
                return reap_waited_child(parent,
                                         child,
                                         status_address,
                                         rusage_address,
                                         linux_result);
            }
            if (child->group_stopped != 0U &&
                (options & LINUX_WUNTRACED) != 0U &&
                child->stop_notified == 0U) {
                status = report_wait_event(parent,
                                           child,
                                           child->wait_status,
                                           status_address,
                                           rusage_address,
                                           linux_result);
                if (status != KERNEL_SCHEDULER_STATUS_OK ||
                    *linux_result != child->tid) {
                    return status;
                }
                child->stop_notified = 1U;
                return KERNEL_SCHEDULER_STATUS_OK;
            }
            if (child->group_stopped == 0U &&
                (options & LINUX_WCONTINUED) != 0U &&
                child->continue_notified != 0U) {
                status = report_wait_event(parent,
                                           child,
                                           LINUX_WAIT_CONTINUED,
                                           status_address,
                                           rusage_address,
                                           linux_result);
                if (status != KERNEL_SCHEDULER_STATUS_OK ||
                    *linux_result != child->tid) {
                    return status;
                }
                child->continue_notified = 0U;
                return KERNEL_SCHEDULER_STATUS_OK;
            }
        }
        if (previous != parent->group_leader->last_child) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
        if (!matching_child) {
            *linux_result = -KERNEL_ECHILD;
            return KERNEL_SCHEDULER_STATUS_OK;
        }
        if ((options & LINUX_WNOHANG) != 0U) {
            *linux_result = 0;
            return KERNEL_SCHEDULER_STATUS_OK;
        }
        status = kernel_scheduler_block_current(&parent->group_leader->child_exit_queue,
                                                0U,
                                                1,
                                                &wake_reason);
        if (status != KERNEL_SCHEDULER_STATUS_OK) {
            return status;
        }
        if (wake_reason == KERNEL_WAIT_SIGNALLED) {
            kernel_signal_note_syscall_restart(parent);
            *linux_result = -KERNEL_ERESTARTSYS;
            return KERNEL_SCHEDULER_STATUS_OK;
        }
    }
}

static enum kernel_scheduler_status cleanup_user_task_resources(
    struct kernel_task *thread);
static uint32_t user_wait_status(const struct kernel_thread_completion *completion);
static enum kernel_scheduler_status reparent_children(struct kernel_task *parent);

int kernel_scheduler_reap_pending(void)
{
    return scheduler.exited_head != 0;
}

enum kernel_scheduler_status kernel_scheduler_reap_one(
    struct kernel_thread_completion *completion)
{
    struct kernel_thread_completion result;
    struct kernel_task *thread;
    struct kernel_task *next;
    uint32_t publish;
    enum kernel_scheduler_status status;

    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED) {
        return KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (completion == 0) {
        return KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT;
    }
    if (riscv_interrupt_is_enabled()) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    if (scheduler.fatal_status != KERNEL_SCHEDULER_STATUS_OK) {
        return scheduler.fatal_status;
    }
    status = validate_current();
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }
    if (scheduler.current != &scheduler.idle) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    status = validate_queues();
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }


    if (scheduler.exited_head == 0) {
        return KERNEL_SCHEDULER_STATUS_EMPTY;
    }

    thread = scheduler.exited_head;
    next = thread->next;
    if (thread->magic != KERNEL_THREAD_MAGIC ||
        thread->state != KERNEL_THREAD_STATE_EXITED ||
        thread->idle != 0U ||
        (thread->physical_address & BOAROS_PAGE_MASK) != 0U ||
        thread->publish_completion > 1U) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    result = thread->completion;
    publish = thread->publish_completion;
    if (result.kind == KERNEL_THREAD_KIND_KERNEL) {
        if (publish == 0U ||
            result.reason != KERNEL_THREAD_EXIT_RETURNED ||
            result.tid != 0 || result.tgid != 0 ||
            result.status != 0U || result.detail != 0U) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
    } else if (result.kind == KERNEL_THREAD_KIND_USER) {
        if (result.reason != KERNEL_THREAD_EXIT_SYSCALL &&
            result.reason != KERNEL_THREAD_EXIT_USER_FAULT &&
            result.reason != KERNEL_THREAD_EXIT_RESOURCE &&
            result.reason != KERNEL_THREAD_EXIT_SIGNAL) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
        if (result.reason == KERNEL_THREAD_EXIT_RESOURCE &&
            result.status != KERNEL_THREAD_RESOURCE_NO_MEMORY) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
        if (result.reason == KERNEL_THREAD_EXIT_SIGNAL &&
            (result.status == 0U || result.status > 64U)) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
        if (publish != 0U &&
            ((thread->tid_owned != 0U &&
             (thread->group_leader == 0 ||
              result.tid != thread->tid ||
              result.tgid != thread->group_leader->tid)) ||
            (thread->tid_owned == 0U &&
             (result.tid <= 0 || result.tgid <= 0)))) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
        status = cleanup_user_task_resources(thread);
        if (status != KERNEL_SCHEDULER_STATUS_OK) {
            return status;
        }
        if (thread->group_leader == thread && thread->group_members > 1U) {
            struct kernel_task *child;
            for (child = thread->first_child; child != 0;
                 child = child->next_sibling)
                if (child->child_creator_tid == thread->tid)
                    child->child_creator_tid = child_reaper_tid(thread, thread);
            /* Keep the group identity until its last member has left. */
            scheduler.exited_head = next;
            if (next == 0) scheduler.exited_tail = 0;
            thread->next = 0;
            thread->state = KERNEL_THREAD_STATE_GROUP_DEAD;
            (void)kernel_wait_queue_wake_all(&thread->group_wait_queue);
            return KERNEL_SCHEDULER_STATUS_EMPTY;
        }
        if (thread->group_leader != 0 && thread->group_leader != thread) {
            struct kernel_task *leader = thread->group_leader;
            struct kernel_task *child;
            /* Children survive their creator thread. Reassign this wait
             * relationship before its TID can be recycled. */
            for (child = leader->first_child; child != 0;
                 child = child->next_sibling)
                if (child->child_creator_tid == thread->tid)
                    child->child_creator_tid = child_reaper_tid(leader, thread);
            leader->user_ticks += thread->user_ticks;
            leader->kernel_ticks += thread->kernel_ticks;
            thread->group_previous->group_next = thread->group_next;
            thread->group_next->group_previous = thread->group_previous;
            leader->group_members--;
            if (leader->group_members == 1U &&
                leader->state == KERNEL_THREAD_STATE_GROUP_DEAD) {
                if (!leader->group_exiting) {
                    leader->completion.reason = thread->completion.reason;
                    leader->completion.status = thread->completion.status;
                    leader->completion.detail = thread->completion.detail;
                }
                leader->wait_status = user_wait_status(&leader->completion);
                leader->state = KERNEL_THREAD_STATE_EXITED;
                exited_append(leader);
                next = thread->next;
            }
            (void)kernel_wait_queue_wake_all(&leader->group_wait_queue);
            thread->group_leader = thread;
            thread->group_members = 1U;
            process_group_initialize(thread);
        }
        if (thread->group_leader == thread &&
            reparent_children(thread) != KERNEL_SCHEDULER_STATUS_OK)
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        /* Reparenting may append orphan zombies behind this tail. */
        next = thread->next;
        if (thread->parent != 0) {
            kernel_signal_notify_child_exit(thread);
            if (kernel_signal_child_autoreap(thread->parent)) {
                child_remove(thread->parent, thread);
            }
        }
        if (thread->parent != 0) {
            status = validate_child_list(thread->parent, thread);
            if (status != KERNEL_SCHEDULER_STATUS_OK) {
                return status;
            }
            scheduler.exited_head = next;
            if (next == 0) {
                scheduler.exited_tail = 0;
            }
            thread->next = 0;
            thread->state = KERNEL_THREAD_STATE_ZOMBIE;
            thread->publish_completion = 0U;
            return KERNEL_SCHEDULER_STATUS_EMPTY;
        }
        if (thread->tid_owned != 0U) {
            if (kernel_pid_release(&scheduler.pid_allocator,
                                   thread->tid) !=
                KERNEL_PID_STATUS_OK) {
                return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
            }
            if (scheduler.init_task == thread) {
                scheduler.init_task = 0;
            }
            thread->tid = 0;
            thread->process_group = 0;
            thread->tid_owned = 0U;
            thread->group_leader = 0;
            thread->group_members = 0U;
        }
    } else {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    (void)physical_page_release(scheduler.allocator,
                              thread->physical_address);
    scheduler.exited_head = next;
    if (next == 0) {
        scheduler.exited_tail = 0;
    }
    if (publish != 0U) {
        *completion = result;
        return KERNEL_SCHEDULER_STATUS_OK;
    }
    return KERNEL_SCHEDULER_STATUS_EMPTY;
}

static void switch_to_fatal_idle(
    enum kernel_scheduler_status status) __attribute__((noreturn));

static void switch_to_fatal_idle(enum kernel_scheduler_status status)
{
    if (scheduler.fatal_status == KERNEL_SCHEDULER_STATUS_OK) {
        scheduler.fatal_status = status;
    }
    if (scheduler.idle_context_saved != 0U &&
        scheduler.current != &scheduler.idle) {
        if (activate_thread_address_space(&scheduler.idle) !=
            KERNEL_SCHEDULER_STATUS_OK) {
            for (;;) {
                __asm__ volatile("wfi");
            }
        }
        scheduler.current = &scheduler.idle;
        riscv_fpu_switch(0, &scheduler.idle.fpu);
        riscv_context_switch(&scheduler.discard_context,
                             &scheduler.idle.context);
    }

    for (;;) {
        __asm__ volatile("wfi");
    }
}

static enum kernel_scheduler_status cleanup_user_task_resources(
    struct kernel_task *thread)
{
    enum kernel_signal_status signal_status;
    enum kernel_mm_status mm_status;
    enum kernel_exec_status exec_status;
    enum kernel_files_status files_status;
    enum kernel_fs_context_status fs_status;

    signal_status = thread->group_leader == thread && thread->group_members > 1U
        ? KERNEL_SIGNAL_STATUS_OK : kernel_signal_release_table(thread);
    if (signal_status != KERNEL_SIGNAL_STATUS_OK) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }

    if (thread->exec_transaction != 0) {
        struct kernel_exec_transaction *transaction =
            thread->exec_transaction;
        struct kernel_heap *heap = transaction->heap;

        /* A sibling may terminate a thread after exec preparation. The
         * prepared image is still owned by this transaction and is aborted. */
        if (transaction->state == KERNEL_EXEC_TRANSACTION_PREPARED)
            transaction->state = KERNEL_EXEC_TRANSACTION_PREPARING;
        exec_status = kernel_exec_transaction_cleanup(transaction);
        if (exec_status != KERNEL_EXEC_STATUS_OK) {
            return exec_status == KERNEL_EXEC_STATUS_CLEANUP_REQUIRED
                       ? KERNEL_SCHEDULER_STATUS_RESOURCE_CLEANUP
                       : KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
        (void)kernel_heap_release(heap, transaction);
        thread->exec_transaction = 0;
    }
    if (thread->files.state == KERNEL_FILES_LIVE ||
        thread->files.state == KERNEL_FILES_CLEANUP) {
        files_status = kernel_files_release(&thread->files);
        if (files_status != KERNEL_FILES_STATUS_OK) {
            return files_status == KERNEL_FILES_STATUS_CLEANUP_REQUIRED
                       ? KERNEL_SCHEDULER_STATUS_RESOURCE_CLEANUP
                       : KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
    }
    if (thread->files.state != KERNEL_FILES_EMPTY &&
        thread->files.state != KERNEL_FILES_RELEASED) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    if (thread->fs.state == KERNEL_FS_CONTEXT_LIVE ||
        thread->fs.state == KERNEL_FS_CONTEXT_CLEANUP) {
        fs_status = kernel_fs_context_release(&thread->fs);
        if (fs_status != KERNEL_FS_CONTEXT_STATUS_OK) {
            return fs_status == KERNEL_FS_CONTEXT_STATUS_CLEANUP_REQUIRED
                       ? KERNEL_SCHEDULER_STATUS_RESOURCE_CLEANUP
                       : KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
    }
    if (thread->fs.state != KERNEL_FS_CONTEXT_EMPTY &&
        thread->fs.state != KERNEL_FS_CONTEXT_RELEASED) {
        return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    }
    if (thread->mm.state == KERNEL_MM_LIVE ||
        thread->mm.state == KERNEL_MM_CLEANUP) {
        mm_status = kernel_mm_release(&thread->mm);
        if (mm_status != KERNEL_MM_STATUS_OK) {
            if (mm_status == KERNEL_MM_STATUS_PAGE_ACCESS) {
                return KERNEL_SCHEDULER_STATUS_PAGE_ACCESS;
            }
            if (mm_status == KERNEL_MM_STATUS_CLEANUP_REQUIRED) {
                return KERNEL_SCHEDULER_STATUS_RESOURCE_CLEANUP;
            }
            return mm_status == KERNEL_MM_STATUS_ADDRESS_SPACE
                       ? KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE
                       : KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
    }
    if (thread->mm.state == KERNEL_MM_RELEASED) {
        process_complete_vfork(thread);
    }
    return (thread->mm.state == KERNEL_MM_RELEASED ||
            (thread->publish_completion == 0U &&
             thread->mm.state == KERNEL_MM_EMPTY))
               ? KERNEL_SCHEDULER_STATUS_OK
               : KERNEL_SCHEDULER_STATUS_INVALID_STATE;
}

static uint32_t fault_wait_status(uint64_t scause)
{
    switch (scause) {
    case 2U:
        return 4U;  /* SIGILL */
    case 3U:
        return 5U;  /* SIGTRAP */
    case 4U:
    case 6U:
        return 7U;  /* SIGBUS */
    default:
        return 11U; /* SIGSEGV */
    }
}

static uint32_t user_wait_status(
    const struct kernel_thread_completion *completion)
{
    if (completion->reason == KERNEL_THREAD_EXIT_SYSCALL) {
        return (uint32_t)((completion->status & UINT64_C(0xff)) << 8U);
    }
    if (completion->reason == KERNEL_THREAD_EXIT_RESOURCE) {
        return 9U; /* SIGKILL */
    }
    if (completion->reason == KERNEL_THREAD_EXIT_SIGNAL) {
        /* A core-producing default action does not mean a dump was written.
         * BoarOS has no core writer; Linux sets bit 7 only after a dump. */
        return (uint32_t)completion->status & UINT32_C(0x7f);
    }
    return fault_wait_status(completion->status);
}

static enum kernel_scheduler_status reparent_children(
    struct kernel_task *parent)
{
    struct kernel_task *new_parent = scheduler.init_task;
    struct kernel_task *child = parent->first_child;
    enum kernel_scheduler_status status;

    status = validate_child_list(parent, 0);
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        return status;
    }

    if (new_parent == parent || new_parent == 0 ||
        new_parent->tid_owned != 1U ||
        new_parent->state == KERNEL_THREAD_STATE_EXITED ||
        new_parent->state == KERNEL_THREAD_STATE_ZOMBIE) {
        new_parent = 0;
    }
    if (new_parent != 0) {
        status = validate_child_list(new_parent, 0);
        if (status != KERNEL_SCHEDULER_STATUS_OK) {
            return status;
        }
    }
    parent->first_child = 0;
    parent->last_child = 0;
    while (child != 0) {
        struct kernel_task *next = child->next_sibling;

        child->parent = 0;
        child->previous_sibling = 0;
        child->next_sibling = 0;
        if (new_parent != 0) {
            child_append(new_parent, child);
            child->child_creator_tid = child_reaper_tid(new_parent, 0);
            if (child->state == KERNEL_THREAD_STATE_ZOMBIE) {
                kernel_signal_notify_child_exit(child);
                if (kernel_signal_child_autoreap(new_parent)) {
                    child_remove(new_parent, child);
                    child->state = KERNEL_THREAD_STATE_EXITED;
                    child->publish_completion = 0U;
                    exited_append(child);
                }
            }
        } else if (child->state == KERNEL_THREAD_STATE_ZOMBIE) {
            child->state = KERNEL_THREAD_STATE_EXITED;
            child->publish_completion = 0U;
            exited_append(child);
        }
        child = next;
    }
    return KERNEL_SCHEDULER_STATUS_OK;
}

static void kernel_thread_finish(
    const struct kernel_thread_completion *completion)
    __attribute__((noreturn));

static void kernel_thread_finish(
    const struct kernel_thread_completion *completion)
{
    struct kernel_task *current;
    struct kernel_task *next;
    enum kernel_scheduler_status cleanup_status =
        KERNEL_SCHEDULER_STATUS_OK;
    enum kernel_scheduler_status status;

    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED ||
        riscv_interrupt_is_enabled()) {
        switch_to_fatal_idle(KERNEL_SCHEDULER_STATUS_INVALID_STATE);
    }
    current = scheduler.current;
    status = validate_current();
    if (status != KERNEL_SCHEDULER_STATUS_OK || current == &scheduler.idle) {
        switch_to_fatal_idle(
            status == KERNEL_SCHEDULER_STATUS_OK
                ? KERNEL_SCHEDULER_STATUS_INVALID_STATE
                : status);
    }
    status = validate_queues();
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        switch_to_fatal_idle(status);
    }

    if (!current->group_exiting) current->completion = *completion;
    if (current->arch.user_mode == 1U) {
        kernel_futex_clear_tid(current);
        if (riscv_sv39_switch_satp(scheduler.kernel_satp) !=
            RISCV_SV39_STATUS_OK) {
            switch_to_fatal_idle(
                KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE);
        }
        cleanup_status = cleanup_user_task_resources(current);
        current->wait_status = user_wait_status(&current->completion);
    }
    (void)cleanup_status;
    current->state = KERNEL_THREAD_STATE_EXITED;
    exited_append(current);

    /* Resume the saved cleanup context even when users remain runnable;
     * zombie publication and group teardown must not depend on idleness. */
    next = &scheduler.idle;
    status = activate_thread_address_space(next);
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        switch_to_fatal_idle(status);
    }
    if (next != &scheduler.idle) {
        next = ready_pop();
        next->state = KERNEL_THREAD_STATE_RUNNING;
    }
    scheduler.current = next;
    /* The dying task's FP state is discarded, but the dispatched task
     * must still have its own image reloaded. */
    riscv_fpu_switch(0, &next->fpu);
    riscv_context_switch(&scheduler.discard_context, &next->context);

    switch_to_fatal_idle(KERNEL_SCHEDULER_STATUS_INVALID_STATE);
}

void kernel_thread_exit(void)
{
    const struct kernel_thread_completion completion = {
        .kind = KERNEL_THREAD_KIND_KERNEL,
        .reason = KERNEL_THREAD_EXIT_RETURNED,
        .tid = 0,
        .tgid = 0,
        .status = 0U,
        .detail = 0U,
    };

    kernel_thread_finish(&completion);
}

void kernel_user_thread_exit(
    enum kernel_thread_exit_reason reason,
    uint64_t status,
    uint64_t detail)
{
    struct kernel_thread_completion completion = {
        .kind = KERNEL_THREAD_KIND_USER,
        .reason = reason,
        .tid = 0,
        .tgid = 0,
        .status = status,
        .detail = detail,
    };

    if (reason != KERNEL_THREAD_EXIT_SYSCALL &&
        reason != KERNEL_THREAD_EXIT_USER_FAULT &&
        reason != KERNEL_THREAD_EXIT_RESOURCE &&
        reason != KERNEL_THREAD_EXIT_SIGNAL) {
        switch_to_fatal_idle(KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT);
    }
    if (reason == KERNEL_THREAD_EXIT_RESOURCE &&
        status != KERNEL_THREAD_RESOURCE_NO_MEMORY) {
        switch_to_fatal_idle(KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT);
    }
    if (reason == KERNEL_THREAD_EXIT_SIGNAL &&
        (status == 0U || status > 64U)) {
        switch_to_fatal_idle(KERNEL_SCHEDULER_STATUS_INVALID_ARGUMENT);
    }
    if (scheduler.current == 0 ||
        scheduler.current->arch.user_mode != 1U) {
        switch_to_fatal_idle(KERNEL_SCHEDULER_STATUS_INVALID_STATE);
    }
    completion.tid = scheduler.current->tid;
    completion.tgid = scheduler.current->group_leader->tid;
    if (reason != KERNEL_THREAD_EXIT_SYSCALL)
        process_group_request_exit(scheduler.current, reason, status, detail);
    kernel_thread_finish(&completion);
}

struct kernel_task *kernel_task_current(void)
{
    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED ||
        scheduler.current == 0 ||
        riscv_current_thread_get() != scheduler.current) {
        return 0;
    }
    return scheduler.current;
}

enum kernel_task_status kernel_task_set_tid_address(
    struct kernel_task *task,
    uint64_t address,
    kernel_pid_t *tid)
{
    if (task == 0 || tid == 0) {
        return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
    }
    task->clear_tid_address = address;
    *tid = task->tid;
    return KERNEL_TASK_STATUS_OK;
}

enum kernel_task_status kernel_task_tid(
    const struct kernel_task *task,
    kernel_pid_t *tid)
{
    if (task == 0 || tid == 0) {
        return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
    }
    if (task->magic != KERNEL_THREAD_MAGIC || task->tid_owned != 1U ||
        task->tid <= 0) {
        return KERNEL_TASK_STATUS_STATE;
    }
    *tid = task->tid;
    return KERNEL_TASK_STATUS_OK;
}

enum kernel_task_status kernel_task_tgid(
    const struct kernel_task *task,
    kernel_pid_t *tgid)
{
    const struct kernel_task *leader;

    if (task == 0 || tgid == 0) {
        return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
    }
    leader = task->group_leader;
    if (task->magic != KERNEL_THREAD_MAGIC || task->tid_owned != 1U ||
        leader == 0 || leader->magic != KERNEL_THREAD_MAGIC ||
        leader->tid_owned != 1U || leader->tid <= 0 ||
        leader->group_leader != leader || leader->group_members == 0U) {
        return KERNEL_TASK_STATUS_STATE;
    }
    *tgid = leader->tid;
    return KERNEL_TASK_STATUS_OK;
}

enum kernel_task_status kernel_task_ppid(
    const struct kernel_task *task,
    kernel_pid_t *ppid)
{
    if (task == 0 || ppid == 0) {
        return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
    }
    if (task->magic != KERNEL_THREAD_MAGIC || task->tid_owned != 1U ||
        task->tid <= 0 || task->arch.user_mode != 1U) {
        return KERNEL_TASK_STATUS_STATE;
    }
    task = task->group_leader;
    if (task->parent == 0) {
        *ppid = 0;
        return KERNEL_TASK_STATUS_OK;
    }
    if (task->parent->magic != KERNEL_THREAD_MAGIC ||
        task->parent->tid_owned != 1U || task->parent->tid <= 0) {
        return KERNEL_TASK_STATUS_STATE;
    }
    *ppid = task->parent->tid;
    return KERNEL_TASK_STATUS_OK;
}

void kernel_task_cpu_ticks(const struct kernel_task *task,
                           uint64_t *user_ticks,
                           uint64_t *kernel_ticks,
                           uint64_t *child_user_ticks,
                           uint64_t *child_kernel_ticks)
{
    if (task == 0 || task->magic != KERNEL_THREAD_MAGIC) {
        return;
    }
    *user_ticks = task->user_ticks;
    *kernel_ticks = task->kernel_ticks;
    *child_user_ticks = task->child_user_ticks;
    *child_kernel_ticks = task->child_kernel_ticks;
    if (task->group_leader != 0) {
        const struct kernel_task *leader = task->group_leader;
        const struct kernel_task *member = leader;
        *user_ticks = 0U;
        *kernel_ticks = 0U;
        do {
            *user_ticks += member->user_ticks;
            *kernel_ticks += member->kernel_ticks;
            member = member->group_next;
        } while (member != leader);
        *child_user_ticks = leader->child_user_ticks;
        *child_kernel_ticks = leader->child_kernel_ticks;
    }
}

enum kernel_task_status kernel_task_mm_borrow(
    const struct kernel_task *task,
    const struct kernel_mm **mm)
{
    if (task == 0 || mm == 0) {
        return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
    }
    if (task != scheduler.current ||
        task->magic != KERNEL_THREAD_MAGIC ||
        task->state != KERNEL_THREAD_STATE_RUNNING ||
        task->arch.user_mode != 1U ||
        task->mm.state != KERNEL_MM_LIVE) {
        return KERNEL_TASK_STATUS_STATE;
    }
    *mm = &task->mm;
    return KERNEL_TASK_STATUS_OK;
}

enum kernel_task_status kernel_task_mm_borrow_mutable(
    struct kernel_task *task,
    struct kernel_mm **mm)
{
    if (task == 0 || mm == 0) {
        return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
    }
    if (task != scheduler.current ||
        task->magic != KERNEL_THREAD_MAGIC ||
        task->state != KERNEL_THREAD_STATE_RUNNING ||
        task->arch.user_mode != 1U ||
        task->mm.state != KERNEL_MM_LIVE) {
        return KERNEL_TASK_STATUS_STATE;
    }
    *mm = &task->mm;
    return KERNEL_TASK_STATUS_OK;
}

enum kernel_task_status validate_task_resource_borrow(
    const struct kernel_task *task)
{
    if (task != scheduler.current ||
        task->magic != KERNEL_THREAD_MAGIC ||
        task->state != KERNEL_THREAD_STATE_RUNNING ||
        task->arch.user_mode != 1U) {
        return KERNEL_TASK_STATUS_STATE;
    }
    if (task->files.state == KERNEL_FILES_EMPTY &&
        task->fs.state == KERNEL_FS_CONTEXT_EMPTY) {
        return KERNEL_TASK_STATUS_RESOURCE_UNAVAILABLE;
    }
    if (!kernel_files_is_live(&task->files) ||
        !kernel_fs_context_is_live(&task->fs)) {
        return KERNEL_TASK_STATUS_STATE;
    }
    return KERNEL_TASK_STATUS_OK;
}

enum kernel_task_status kernel_task_files_borrow(
    struct kernel_task *task,
    struct kernel_files **files)
{
    enum kernel_task_status status;

    if (task == 0 || files == 0) {
        return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
    }
    status = validate_task_resource_borrow(task);
    if (status != KERNEL_TASK_STATUS_OK) {
        return status;
    }
    *files = &task->files;
    return KERNEL_TASK_STATUS_OK;
}

enum kernel_task_status kernel_task_fs_context_borrow(
    const struct kernel_task *task,
    const struct kernel_fs_context **fs)
{
    enum kernel_task_status status;

    if (task == 0 || fs == 0) {
        return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
    }
    status = validate_task_resource_borrow(task);
    if (status != KERNEL_TASK_STATUS_OK) {
        return status;
    }
    *fs = &task->fs;
    return KERNEL_TASK_STATUS_OK;
}
