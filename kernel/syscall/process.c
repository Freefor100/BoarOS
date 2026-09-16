#include "private.h"

#include <kernel/errno.h>
#include <kernel/exec.h>
#include <kernel/mm.h>
#include <kernel/signal.h>
#include <kernel/task.h>
#include <kernel/uaccess.h>

#include <stddef.h>
#include <stdint.h>

#define LINUX_CLONE_SIGNAL_MASK UINT64_C(0xff)
#define LINUX_SIGCHLD UINT64_C(17)
#define LINUX_CLONE_VM UINT64_C(0x100)
#define LINUX_CLONE_VFORK UINT64_C(0x4000)
#define LINUX_CLONE_KNOWN_FLAGS UINT64_C(0x3ffffffff)

enum kernel_syscall_status syscall_handle_prlimit64(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    struct kernel_rlimit64 before;
    struct kernel_rlimit64 replacement;
    struct kernel_mm *mm;
    struct kernel_task *target;
    int64_t pid = (int64_t)(int32_t)(uint32_t)request->arguments[0];
    uint32_t resource = (uint32_t)request->arguments[1];
    uint64_t new_address = request->arguments[2];
    uint64_t old_address = request->arguments[3];
    size_t copied = 0U;
    enum kernel_uaccess_status access_status;

    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    if (kernel_task_mm_borrow_mutable(caller, &mm) != KERNEL_TASK_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    if (new_address != 0U) {
        access_status = kernel_copy_from_user(mm, &replacement, new_address,
                                              sizeof(replacement), &copied);
        if (access_status == KERNEL_UACCESS_STATUS_FAULT) {
            decoded->value = -KERNEL_EFAULT;
            return KERNEL_SYSCALL_STATUS_OK;
        }
        if (access_status != KERNEL_UACCESS_STATUS_OK ||
            copied != sizeof(replacement))
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    target = pid == 0 ? caller :
             pid > 0 && pid <= INT32_MAX
                 ? kernel_signal_find_by_tid((kernel_pid_t)pid) : 0;
    if (target == 0) {
        decoded->value = -KERNEL_ESRCH;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (resource >= 16U) {
        decoded->value = -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (resource != KERNEL_RLIMIT_NOFILE &&
        resource != KERNEL_RLIMIT_STACK) {
        decoded->value = -KERNEL_ENOTSUP;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (kernel_task_get_rlimit(target, resource, &before) !=
        KERNEL_TASK_STATUS_OK) return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    if (new_address != 0U) {
        uint64_t capacity = resource == KERNEL_RLIMIT_NOFILE
                                ? KERNEL_RLIMIT_NOFILE_CAP
                                : KERNEL_RLIMIT_STACK_CAP;
        if (replacement.current > replacement.maximum) {
            decoded->value = -KERNEL_EINVAL;
            return KERNEL_SYSCALL_STATUS_OK;
        }
        /* All current user tasks run with root credentials. The hard cap
         * bounds what this kernel can actually enforce. */
        if (replacement.maximum > capacity) {
            decoded->value = -KERNEL_EPERM;
            return KERNEL_SYSCALL_STATUS_OK;
        }
        if (kernel_task_set_rlimit(target, resource, &replacement) !=
            KERNEL_TASK_STATUS_OK) return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    if (old_address != 0U) {
        copied = 0U;
        access_status = kernel_copy_to_user(mm, old_address, &before,
                                            sizeof(before), &copied);
        if (access_status == KERNEL_UACCESS_STATUS_FAULT) {
            decoded->value = -KERNEL_EFAULT;
            return KERNEL_SYSCALL_STATUS_OK;
        }
        if (access_status != KERNEL_UACCESS_STATUS_OK ||
            copied != sizeof(before))
            return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    decoded->value = 0;
    return KERNEL_SYSCALL_STATUS_OK;
}

enum kernel_syscall_status syscall_handle_set_tid_address(
    struct kernel_task *caller,
    uint64_t address,
    struct kernel_syscall_result *decoded)
{
    kernel_pid_t tid;

    if (kernel_task_set_tid_address(caller, address, &tid) !=
        KERNEL_TASK_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = tid;
    return KERNEL_SYSCALL_STATUS_OK;
}


enum kernel_syscall_status syscall_handle_execve(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    int64_t linux_result;

    if (kernel_execve_prepare(caller,
                              request->arguments[0],
                              request->arguments[1],
                              request->arguments[2],
                              &linux_result) != KERNEL_EXEC_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    decoded->action = linux_result == 0
                          ? KERNEL_SYSCALL_ACTION_EXEC
                          : KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = linux_result;
    return KERNEL_SYSCALL_STATUS_OK;
}


/*
 * clone(220) accepts the process forms: fork with an optional custom
 * child stack, and vfork (CLONE_VM|CLONE_VFORK) which shares the parent
 * address space and suspends the parent until the child execs or exits.
 * The thread form shares MM/files/fs/dispositions with its group and
 * accepts the TLS and TID lifecycle flags. Other resource-sharing forms
 * require their own complete lifecycle before they can be enabled.
 */
void syscall_decode_clone(const struct kernel_syscall_request *request,
                         struct kernel_syscall_result *decoded)
{
    uint64_t flags = request->arguments[0];
    const uint64_t thread_required = UINT64_C(0x100) | UINT64_C(0x200) |
        UINT64_C(0x400) | UINT64_C(0x800) | UINT64_C(0x10000);
    const uint64_t thread_optional = UINT64_C(0x40000) | UINT64_C(0x80000) |
        UINT64_C(0x100000) | UINT64_C(0x200000) | UINT64_C(0x400000) |
        UINT64_C(0x1000000);

    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    if ((flags & ~LINUX_CLONE_KNOWN_FLAGS) != 0U ||
        ((flags & UINT64_C(0x10000)) && !(flags & UINT64_C(0x800))) ||
        ((flags & UINT64_C(0x800)) && !(flags & LINUX_CLONE_VM))) {
        decoded->value = -KERNEL_EINVAL;
        return;
    }
    if ((flags & thread_required) == thread_required &&
        (flags & ~(thread_required | thread_optional)) == 0U) {
        decoded->action = KERNEL_SYSCALL_ACTION_CLONE;
        decoded->value = 0;
        return;
    }
    if ((flags & LINUX_CLONE_SIGNAL_MASK) != LINUX_SIGCHLD &&
        !(flags & UINT64_C(0x10000))) {
        decoded->value = -KERNEL_EINVAL;
        return;
    }
    if (flags != LINUX_SIGCHLD &&
        flags != (LINUX_SIGCHLD | LINUX_CLONE_VM | LINUX_CLONE_VFORK)) {
        decoded->value = -KERNEL_ENOTSUP;
        return;
    }
    /* Without the CLONE_*SETTID/SETTLS flag bits Linux
     * ignores the remaining arguments; musl's fork passes only two. */
    decoded->action = KERNEL_SYSCALL_ACTION_CLONE;
    decoded->value = 0;
}
