#include <arch/riscv/context.h>
#include <arch/riscv/mm.h>
#include <arch/riscv/process.h>
#include <arch/riscv/sv39.h>
#include <arch/riscv/thread.h>
#include <arch/riscv/trap.h>
#include <kernel/errno.h>
#include <kernel/exec.h>
#include <kernel/files.h>
#include <kernel/fs_context.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
#include <kernel/tick.h>
#include <kernel/page.h>
#include <kernel/physical_page.h>
#include <kernel/pid.h>
#include <kernel/scheduler.h>
#include <kernel/task.h>
#include <kernel/uaccess.h>

#include <stddef.h>
#include <stdint.h>

#include "../exec_internal.h"
#include "private.h"

#define LINUX_WNOHANG UINT32_C(0x00000001)
#define LINUX_WUNTRACED UINT32_C(0x00000002)
#define LINUX_WCONTINUED UINT32_C(0x00000008)
#define LINUX___WNOTHREAD UINT32_C(0x20000000)
#define LINUX___WALL UINT32_C(0x40000000)
#define LINUX___WCLONE UINT32_C(0x80000000)
#define LINUX_WAIT4_SUPPORTED_OPTIONS \
    (LINUX_WNOHANG | LINUX_WUNTRACED | LINUX_WCONTINUED | \
     LINUX___WNOTHREAD | LINUX___WALL | LINUX___WCLONE)

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
            child->state > KERNEL_THREAD_STATE_ZOMBIE) {
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

static void wake_waiting_parent(struct kernel_task *child)
{
    struct kernel_task *parent = child->parent;

    if (parent != 0 && parent->state == KERNEL_THREAD_STATE_BLOCKED) {
        blocked_unlink(parent);
        parent->state = KERNEL_THREAD_STATE_READY;
        ready_append(parent);
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

static int release_clone_resources(struct kernel_task *thread)
{
    int cleanup_failed = 0;

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
    int cleanup_failed = release_clone_resources(thread);

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
        if (physical_page_release(scheduler.allocator,
                                  thread->physical_address) !=
            PHYSICAL_PAGE_STATUS_OK) {
            if (scheduler.cleanup_page_owned == 0U) {
                scheduler.cleanup_page_address = thread->physical_address;
                scheduler.cleanup_page_owned = 1U;
            } else {
                cleanup_failed = 1;
            }
        }
    }
    if (cleanup_failed) {
        queue_abandoned_clone(thread);
    }
    *linux_result = linux_failure;
    return scheduler_failure;
}

enum kernel_scheduler_status riscv_process_clone_current(
    const struct riscv_trap_frame *parent_frame,
    uint64_t child_stack,
    int64_t *linux_result)
{
    struct kernel_task *parent;
    struct kernel_task *child;
    struct riscv_trap_frame *child_frame;
    uint64_t physical_address;
    uint64_t child_satp;
    uintptr_t stack_low;
    void *page;
    kernel_pid_t tid;
    enum physical_page_status page_status;
    enum kernel_mm_status mm_status;
    enum kernel_files_status files_status;
    enum kernel_fs_context_status fs_status;
    enum kernel_pid_status pid_status;
    enum riscv_context_status context_status;
    enum kernel_scheduler_status status;

    if (scheduler.initialized != KERNEL_SCHEDULER_INITIALIZED) {
        return KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED;
    }
    if (parent_frame == 0 || linux_result == 0 || child_stack != 0U ||
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

    mm_status = kernel_mm_fork(&child->mm, &parent->mm);
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
        files_status = kernel_files_fork(&child->files,
                                         &parent->files);
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
        fs_status = kernel_fs_context_fork(&child->fs, &parent->fs);
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
    child->publish_completion = 0U;
    child->completion.tid = tid;
    child->completion.tgid = tid;
    mm_status = riscv_kernel_mm_satp(&child->mm, &child_satp);
    if (mm_status != KERNEL_MM_STATUS_OK) {
        return finish_clone_failure(
            child, -KERNEL_EAGAIN, linux_result,
            KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE);
    }
    child->arch.satp = child_satp;
    child_frame = (struct riscv_trap_frame *)(child->stack_high -
                                               sizeof(*child_frame));
    if ((uintptr_t)child_frame < child->stack_low) {
        return finish_clone_failure(
            child, -KERNEL_EAGAIN, linux_result,
            KERNEL_SCHEDULER_STATUS_INVALID_STATE);
    }
    *child_frame = *parent_frame;
    child_frame->a0 = 0U;
    child_frame->sepc = parent_frame->sepc + 4U;
    child_frame->scause = 0U;
    child_frame->stval = 0U;
    child_frame->kernel_tp = (uintptr_t)child;
    context_status = riscv_context_init_user(&child->context,
                                             (uintptr_t)child_frame,
                                             child);
    if (context_status != RISCV_CONTEXT_STATUS_OK) {
        return finish_clone_failure(
            child, -KERNEL_EAGAIN, linux_result,
            KERNEL_SCHEDULER_STATUS_INVALID_STATE);
    }

    child_append(parent, child);
    child->state = KERNEL_THREAD_STATE_READY;
    ready_append(child);
    *linux_result = tid;
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
        child->parent != parent || child->tid_owned != 1U || pid <= 0 ||
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
    parent->child_user_ticks += user_ticks;
    parent->child_kernel_ticks += kernel_ticks;
    child_remove(parent, child);
    if (scheduler.init_task == child) {
        scheduler.init_task = 0;
    }
    child->tid = 0;
    child->process_group = 0;
    child->tid_owned = 0U;
    child->group_leader = 0;
    child->group_members = 0U;
    child->publish_completion = 0U;
    if (physical_page_release(scheduler.allocator,
                              child->physical_address) !=
        PHYSICAL_PAGE_STATUS_OK) {
        child->state = KERNEL_THREAD_STATE_EXITED;
        exited_append(child);
    }

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

enum kernel_scheduler_status kernel_scheduler_wait4_current(
    int64_t pid,
    uint64_t status_address,
    uint32_t options,
    uint64_t rusage_address,
    int64_t *linux_result)
{
    struct kernel_task *parent;
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
        status = validate_child_endpoints(parent);
        if (status != KERNEL_SCHEDULER_STATUS_OK) {
            return status;
        }
        for (child = parent->first_child;
             child != 0;
             child = child->next_sibling) {
            if (++child_count > KERNEL_PID_LIMIT || child == parent ||
                child->magic != KERNEL_THREAD_MAGIC ||
                child->idle != 0U || child->arch.user_mode != 1U ||
                child->tid_owned != 1U || child->tid <= 0 ||
                child->parent != parent ||
                child->previous_sibling != previous ||
                child->publish_completion != 0U ||
                child->state < KERNEL_THREAD_STATE_READY ||
                child->state > KERNEL_THREAD_STATE_ZOMBIE) {
                return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
            }
            previous = child;
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
        }
        if (previous != parent->last_child) {
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
        parent->state = KERNEL_THREAD_STATE_BLOCKED;
        blocked_append(parent);
        status = scheduler_switch_current_away(parent);
        if (status != KERNEL_SCHEDULER_STATUS_OK) {
            return status;
        }
    }
}

static enum kernel_scheduler_status cleanup_user_task_resources(
    struct kernel_task *thread);

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

    if (scheduler.cleanup_page_owned != 0U) {
        if (physical_page_release(scheduler.allocator,
                                  scheduler.cleanup_page_address) !=
            PHYSICAL_PAGE_STATUS_OK) {
            return KERNEL_SCHEDULER_STATUS_PAGE_RELEASE;
        }
        scheduler.cleanup_page_address = 0U;
        scheduler.cleanup_page_owned = 0U;
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
            wake_waiting_parent(thread);
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
    if (physical_page_release(scheduler.allocator,
                              thread->physical_address) !=
        PHYSICAL_PAGE_STATUS_OK) {
        return KERNEL_SCHEDULER_STATUS_PAGE_RELEASE;
    }
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
    enum kernel_mm_status mm_status;
    enum kernel_exec_status exec_status;
    enum kernel_files_status files_status;
    enum kernel_fs_context_status fs_status;

    if (thread->exec_transaction != 0) {
        struct kernel_exec_transaction *transaction =
            thread->exec_transaction;
        struct kernel_heap *heap = transaction->heap;

        if (transaction->state == KERNEL_EXEC_TRANSACTION_PREPARED) {
            return KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
        exec_status = kernel_exec_transaction_cleanup(transaction);
        if (exec_status != KERNEL_EXEC_STATUS_OK) {
            return exec_status == KERNEL_EXEC_STATUS_CLEANUP_REQUIRED
                       ? KERNEL_SCHEDULER_STATUS_RESOURCE_CLEANUP
                       : KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
        if (kernel_heap_release(heap, transaction) !=
            KERNEL_HEAP_STATUS_OK) {
            return KERNEL_SCHEDULER_STATUS_RESOURCE_CLEANUP;
        }
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
            if (mm_status == KERNEL_MM_STATUS_PAGE_RELEASE ||
                mm_status == KERNEL_MM_STATUS_CLEANUP_REQUIRED) {
                return KERNEL_SCHEDULER_STATUS_PAGE_RELEASE;
            }
            return mm_status == KERNEL_MM_STATUS_ADDRESS_SPACE
                       ? KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE
                       : KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        }
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
            if (child->state == KERNEL_THREAD_STATE_ZOMBIE) {
                wake_waiting_parent(child);
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

    current->completion = *completion;
    if (current->arch.user_mode == 1U) {
        if (riscv_sv39_switch_satp(scheduler.kernel_satp) !=
            RISCV_SV39_STATUS_OK) {
            switch_to_fatal_idle(
                KERNEL_SCHEDULER_STATUS_ADDRESS_SPACE);
        }
        status = reparent_children(current);
        if (status != KERNEL_SCHEDULER_STATUS_OK) {
            switch_to_fatal_idle(status);
        }
        cleanup_status = cleanup_user_task_resources(current);
        current->wait_status = user_wait_status(completion);
    }
    if (cleanup_status == KERNEL_SCHEDULER_STATUS_OK &&
        current->parent != 0) {
        current->state = KERNEL_THREAD_STATE_ZOMBIE;
        current->publish_completion = 0U;
        wake_waiting_parent(current);
    } else {
        current->state = KERNEL_THREAD_STATE_EXITED;
        exited_append(current);
    }

    if (scheduler.ready_head == 0) {
        next = &scheduler.idle;
    } else {
        next = scheduler.ready_head;
    }
    status = activate_thread_address_space(next);
    if (status != KERNEL_SCHEDULER_STATUS_OK) {
        switch_to_fatal_idle(status);
    }
    if (next != &scheduler.idle) {
        next = ready_pop();
        next->state = KERNEL_THREAD_STATE_RUNNING;
    }
    scheduler.current = next;
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
