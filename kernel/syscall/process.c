#include "private.h"

#include <kernel/errno.h>
#include <kernel/exec.h>
#include <kernel/task.h>

#include <stddef.h>
#include <stdint.h>

#define LINUX_CLONE_SIGNAL_MASK UINT64_C(0xff)
#define LINUX_SIGCHLD UINT64_C(17)
#define LINUX_CLONE_VM UINT64_C(0x100)
#define LINUX_CLONE_VFORK UINT64_C(0x4000)
#define LINUX_CLONE_KNOWN_FLAGS UINT64_C(0x3ffffffff)

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
 * Tid-pointer and TLS arguments belong to the thread model and stay
 * ENOTSUP; CLONE_VFORK without CLONE_VM has distinct Linux semantics and
 * is rejected until needed.
 */
void syscall_decode_clone(const struct kernel_syscall_request *request,
                         struct kernel_syscall_result *decoded)
{
    uint64_t flags = request->arguments[0];

    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    if ((flags & ~LINUX_CLONE_KNOWN_FLAGS) != 0U ||
        (flags & LINUX_CLONE_SIGNAL_MASK) != LINUX_SIGCHLD) {
        decoded->value = -KERNEL_EINVAL;
        return;
    }
    if (flags != LINUX_SIGCHLD &&
        flags != (LINUX_SIGCHLD | LINUX_CLONE_VM | LINUX_CLONE_VFORK)) {
        decoded->value = -KERNEL_ENOTSUP;
        return;
    }
    /* Without the CLONE_*SETTID/SETTLS flag bits (rejected above) Linux
     * ignores the remaining arguments; musl's fork passes only two. */
    decoded->action = KERNEL_SYSCALL_ACTION_CLONE;
    decoded->value = 0;
}
