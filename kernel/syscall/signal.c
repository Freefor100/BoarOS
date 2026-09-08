#include "private.h"

#include <kernel/errno.h>
#include <kernel/mm.h>
#include <kernel/signal.h>
#include <kernel/task.h>
#include <kernel/uaccess.h>

#include <stddef.h>
#include <stdint.h>

#define LINUX_SIGSET_SIZE 8U
#define LINUX_SYSCALL_KILL 129U
#define LINUX_SYSCALL_TGKILL 131U

/* These helpers expose the actual copy result. Only handlers map errno. */
static enum kernel_uaccess_status signal_copy_from_user(
    struct kernel_mm *mm, uint64_t address, void *destination, size_t size)
{
    size_t copied = 0U;
    enum kernel_uaccess_status status =
        kernel_copy_from_user(mm, destination, address, size, &copied);

    return status == KERNEL_UACCESS_STATUS_OK && copied != size
               ? KERNEL_UACCESS_STATUS_STATE : status;
}

static enum kernel_uaccess_status signal_copy_to_user(
    struct kernel_mm *mm, uint64_t address, const void *source, size_t size)
{
    size_t copied = 0U;
    enum kernel_uaccess_status status =
        kernel_copy_to_user(mm, address, source, size, &copied);

    return status == KERNEL_UACCESS_STATUS_OK && copied != size
               ? KERNEL_UACCESS_STATUS_STATE : status;
}

static enum kernel_syscall_status signal_copy_error(
    enum kernel_uaccess_status status, struct kernel_syscall_result *decoded)
{
    if (status != KERNEL_UACCESS_STATUS_FAULT) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    decoded->value = -KERNEL_EFAULT;
    return KERNEL_SYSCALL_STATUS_OK;
}

static enum kernel_syscall_status signal_result(
    enum kernel_signal_status status, struct kernel_syscall_result *decoded)
{
    if (status == KERNEL_SIGNAL_STATUS_OK) {
        decoded->value = 0;
    } else if (status == KERNEL_SIGNAL_STATUS_INVALID_ARGUMENT) {
        decoded->value = -KERNEL_EINVAL;
    } else if (status == KERNEL_SIGNAL_STATUS_NO_MEMORY) {
        decoded->value = -KERNEL_ENOMEM;
    } else {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    return KERNEL_SYSCALL_STATUS_OK;
}

enum kernel_syscall_status syscall_handle_rt_sigaction(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    uint32_t sig = (uint32_t)request->arguments[0];
    uint64_t input = request->arguments[1];
    uint64_t output = request->arguments[2];
    struct kernel_linux_sigaction action;
    struct kernel_linux_sigaction old_action;
    struct kernel_mm *mm;
    enum kernel_uaccess_status copy_status;
    enum kernel_signal_status status;

    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    if (request->arguments[3] != LINUX_SIGSET_SIZE || sig == 0U ||
        sig > KERNEL_SIGNAL_COUNT || sig == 9U || sig == 19U) {
        decoded->value = -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (kernel_task_mm_borrow_mutable(caller, &mm) != KERNEL_TASK_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    /* Snapshot before writing oldact: act and oldact may alias. Linux
     * commits the action before copying oldact out, so output EFAULT is
     * not a rollback of an otherwise valid installation. */
    if (input != 0U) {
        copy_status = signal_copy_from_user(mm, input, &action, sizeof(action));
        if (copy_status != KERNEL_UACCESS_STATUS_OK) {
            return signal_copy_error(copy_status, decoded);
        }
    }
    status = kernel_signal_get_action(caller, sig, &old_action);
    if (status != KERNEL_SIGNAL_STATUS_OK) {
        return signal_result(status, decoded);
    }
    if (input != 0U) {
        status = kernel_signal_set_action(caller, sig, &action);
        if (status != KERNEL_SIGNAL_STATUS_OK) {
            return signal_result(status, decoded);
        }
    }
    if (output != 0U) {
        copy_status = signal_copy_to_user(mm, output, &old_action,
                                          sizeof(old_action));
        if (copy_status != KERNEL_UACCESS_STATUS_OK) {
            return signal_copy_error(copy_status, decoded);
        }
    }
    return signal_result(KERNEL_SIGNAL_STATUS_OK, decoded);
}

enum kernel_syscall_status syscall_handle_rt_sigprocmask(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    uint32_t how = (uint32_t)request->arguments[0];
    uint64_t input = request->arguments[1];
    uint64_t output = request->arguments[2];
    uint64_t mask;
    uint64_t old_mask;
    struct kernel_mm *mm;
    enum kernel_uaccess_status copy_status;
    enum kernel_signal_status status;

    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    if (request->arguments[3] != LINUX_SIGSET_SIZE ||
        (input != 0U && how > LINUX_SIG_SETMASK)) {
        decoded->value = -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (kernel_task_mm_borrow_mutable(caller, &mm) != KERNEL_TASK_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    if (input != 0U) {
        copy_status = signal_copy_from_user(mm, input, &mask, sizeof(mask));
        if (copy_status != KERNEL_UACCESS_STATUS_OK) {
            return signal_copy_error(copy_status, decoded);
        }
    }
    status = kernel_signal_update_blocked(caller, how,
                                          input != 0U ? &mask : 0, &old_mask);
    if (status != KERNEL_SIGNAL_STATUS_OK) {
        return signal_result(status, decoded);
    }
    if (output != 0U) {
        copy_status = signal_copy_to_user(mm, output, &old_mask, sizeof(old_mask));
        if (copy_status != KERNEL_UACCESS_STATUS_OK) {
            return signal_copy_error(copy_status, decoded);
        }
    }
    return signal_result(KERNEL_SIGNAL_STATUS_OK, decoded);
}

enum kernel_syscall_status syscall_handle_rt_sigpending(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    struct kernel_mm *mm;
    uint64_t pending;
    enum kernel_uaccess_status copy_status;
    enum kernel_signal_status status;

    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    if (request->arguments[1] != LINUX_SIGSET_SIZE) {
        decoded->value = -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (kernel_task_mm_borrow_mutable(caller, &mm) != KERNEL_TASK_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    status = kernel_signal_get_pending(caller, &pending);
    if (status != KERNEL_SIGNAL_STATUS_OK) {
        return signal_result(status, decoded);
    }
    copy_status = signal_copy_to_user(mm, request->arguments[0],
                                      &pending, sizeof(pending));
    if (copy_status != KERNEL_UACCESS_STATUS_OK) {
        return signal_copy_error(copy_status, decoded);
    }
    return signal_result(KERNEL_SIGNAL_STATUS_OK, decoded);
}

enum kernel_syscall_status syscall_handle_rt_sigsuspend(
    struct kernel_task *caller,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    struct kernel_mm *mm;
    uint64_t mask;
    enum kernel_uaccess_status copy_status;
    enum kernel_signal_status status;

    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    if (request->arguments[1] != LINUX_SIGSET_SIZE) {
        decoded->value = -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (kernel_task_mm_borrow_mutable(caller, &mm) != KERNEL_TASK_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    copy_status = signal_copy_from_user(mm, request->arguments[0],
                                        &mask, sizeof(mask));
    if (copy_status != KERNEL_UACCESS_STATUS_OK) {
        return signal_copy_error(copy_status, decoded);
    }
    status = kernel_signal_suspend(caller, mask);
    if (status != KERNEL_SIGNAL_STATUS_OK) {
        return signal_result(status, decoded);
    }
    decoded->value = -KERNEL_EINTR;
    return KERNEL_SYSCALL_STATUS_OK;
}

static int64_t syscall_signal_pid(uint64_t value)
{
    return (int64_t)(int32_t)(uint32_t)value;
}

enum kernel_syscall_status syscall_handle_signal_send(
    struct kernel_task *caller,
    uint32_t kind,
    const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    int64_t pid = syscall_signal_pid(request->arguments[0]);
    uint32_t sig = (uint32_t)request->arguments[kind == LINUX_SYSCALL_TGKILL
                                                   ? 2U
                                                   : 1U];
    kernel_pid_t sender_tid;
    uint32_t sent;
    enum kernel_task_status task_status;

    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    if (sig > KERNEL_SIGNAL_COUNT ||
        (kind != LINUX_SYSCALL_KILL && pid <= 0) ||
        (kind == LINUX_SYSCALL_TGKILL &&
         syscall_signal_pid(request->arguments[1]) <= 0)) {
        decoded->value = -KERNEL_EINVAL;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (kind == LINUX_SYSCALL_KILL && sig == 0U) {
        sent = kernel_signal_resolve_targets(caller, pid);
        decoded->value = sent == 0U ? -KERNEL_ESRCH : 0;
        return KERNEL_SYSCALL_STATUS_OK;
    }
    task_status = kernel_task_tid(caller, &sender_tid);
    if (task_status != KERNEL_TASK_STATUS_OK) {
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    }
    if (kind == LINUX_SYSCALL_KILL) {
        sent = kernel_signal_send_targets(caller,
                                          pid,
                                          sig,
                                          sender_tid);
    } else {
        kernel_pid_t tgid = kind == LINUX_SYSCALL_TGKILL
                                ? (kernel_pid_t)syscall_signal_pid(
                                      request->arguments[0])
                                : 0;
        kernel_pid_t tid = (kernel_pid_t)syscall_signal_pid(
            request->arguments[kind == LINUX_SYSCALL_TGKILL ? 1U : 0U]);

        sent = kernel_signal_send_thread(caller,
                                          tgid,
                                          tid,
                                          sig,
                                          sender_tid);
    }
    decoded->value = sent == 0U ? -KERNEL_ESRCH : 0;
    return KERNEL_SYSCALL_STATUS_OK;
}
