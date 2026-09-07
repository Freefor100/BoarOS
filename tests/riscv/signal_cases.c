#include <kernel/errno.h>
#include <kernel/pid.h>
#include <kernel/signal.h>
#include <kernel/syscall.h>
#include <kernel/task.h>
#include <kernel/uaccess.h>

#include <stdint.h>
#include <string.h>

static struct kernel_linux_sigaction user_action = {
    .handler = UINT64_C(0x12345678),
    .flags = LINUX_SA_RESTART,
    .mask = UINT64_C(0x55),
};
static struct kernel_linux_sigaction copied_action;
static uint64_t user_mask = UINT64_C(0x1234);
static uint64_t copied_mask;
static uint64_t copied_pending;
static enum kernel_signal_status signal_status = KERNEL_SIGNAL_STATUS_OK;
static struct kernel_linux_sigaction old_action = {
    .handler = KERNEL_SIGNAL_DFL,
    .flags = 0,
    .mask = 0,
};
static uint64_t old_blocked = UINT64_C(0x20);
static uint64_t pending = UINT64_C(0x400);
static uint32_t set_sig;
static struct kernel_linux_sigaction set_action_value;
static uint32_t update_how;
static uint64_t update_mask;
static int64_t sent_pid;
static kernel_pid_t sent_tgid;
static kernel_pid_t sent_tid;
static uint32_t sent_sig;
static kernel_pid_t sent_sender;
static uint32_t sent_count = 1U;

enum kernel_task_status __wrap_kernel_task_mm_borrow_mutable(
    struct kernel_task *task,
    struct kernel_mm **mm)
{
    if (task != (struct kernel_task *)(uintptr_t)1U || mm == 0) {
        return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
    }
    *mm = (struct kernel_mm *)(uintptr_t)2U;
    return KERNEL_TASK_STATUS_OK;
}

enum kernel_uaccess_status __wrap_kernel_copy_from_user(
    struct kernel_mm *mm,
    void *kernel_destination,
    uint64_t user_source,
    size_t size,
    size_t *bytes_copied)
{
    if (mm != (struct kernel_mm *)(uintptr_t)2U ||
        kernel_destination == 0 || bytes_copied == 0) {
        return KERNEL_UACCESS_STATUS_INVALID_ARGUMENT;
    }
    if (user_source == UINT64_C(0x1000) &&
        size == sizeof(user_action)) {
        memcpy(kernel_destination, &user_action, size);
    } else if (user_source == UINT64_C(0x3000) && size == sizeof(user_mask)) {
        memcpy(kernel_destination, &user_mask, size);
    } else {
        return KERNEL_UACCESS_STATUS_FAULT;
    }
    *bytes_copied = size;
    return KERNEL_UACCESS_STATUS_OK;
}

enum kernel_uaccess_status __wrap_kernel_copy_to_user(
    struct kernel_mm *mm,
    uint64_t user_destination,
    const void *kernel_source,
    size_t size,
    size_t *bytes_copied)
{
    if (mm != (struct kernel_mm *)(uintptr_t)2U || kernel_source == 0 ||
        bytes_copied == 0) {
        return KERNEL_UACCESS_STATUS_INVALID_ARGUMENT;
    }
    if (user_destination == UINT64_C(0x2000) &&
        size == sizeof(copied_action)) {
        memcpy(&copied_action, kernel_source, size);
    } else if (user_destination == UINT64_C(0x4000) &&
               size == sizeof(copied_mask)) {
        memcpy(&copied_mask, kernel_source, size);
    } else if (user_destination == UINT64_C(0x5000) &&
               size == sizeof(copied_pending)) {
        memcpy(&copied_pending, kernel_source, size);
    } else {
        return KERNEL_UACCESS_STATUS_FAULT;
    }
    *bytes_copied = size;
    return KERNEL_UACCESS_STATUS_OK;
}

enum kernel_signal_status __wrap_kernel_signal_get_action(
    struct kernel_task *task,
    uint32_t sig,
    struct kernel_linux_sigaction *action)
{
    if (task != (struct kernel_task *)(uintptr_t)1U || sig != 10U ||
        action == 0) {
        return KERNEL_SIGNAL_STATUS_INVALID_ARGUMENT;
    }
    *action = old_action;
    return signal_status;
}

enum kernel_signal_status __wrap_kernel_signal_set_action(
    struct kernel_task *task,
    uint32_t sig,
    const struct kernel_linux_sigaction *action)
{
    if (task != (struct kernel_task *)(uintptr_t)1U || action == 0) {
        return KERNEL_SIGNAL_STATUS_INVALID_ARGUMENT;
    }
    set_sig = sig;
    set_action_value = *action;
    return signal_status;
}

enum kernel_signal_status __wrap_kernel_signal_get_blocked(
    const struct kernel_task *task,
    uint64_t *blocked)
{
    if (task != (const struct kernel_task *)(uintptr_t)1U || blocked == 0) {
        return KERNEL_SIGNAL_STATUS_INVALID_ARGUMENT;
    }
    *blocked = old_blocked;
    return signal_status;
}

enum kernel_signal_status __wrap_kernel_signal_update_blocked(
    struct kernel_task *task,
    uint32_t how,
    const uint64_t *new_mask,
    uint64_t *old_mask)
{
    if (task != (struct kernel_task *)(uintptr_t)1U || new_mask == 0) {
        return KERNEL_SIGNAL_STATUS_INVALID_ARGUMENT;
    }
    update_how = how;
    update_mask = *new_mask;
    if (old_mask != 0) {
        *old_mask = old_blocked;
    }
    return signal_status;
}

enum kernel_signal_status __wrap_kernel_signal_get_pending(
    const struct kernel_task *task,
    uint64_t *result)
{
    if (task != (const struct kernel_task *)(uintptr_t)1U || result == 0) {
        return KERNEL_SIGNAL_STATUS_INVALID_ARGUMENT;
    }
    *result = pending;
    return signal_status;
}

uint32_t __wrap_kernel_signal_send_targets(struct kernel_task *caller,
                                           int64_t pid,
                                           uint32_t sig,
                                           kernel_pid_t sender_tid)
{
    if (caller != (struct kernel_task *)(uintptr_t)1U) {
        return 0U;
    }
    sent_pid = pid;
    sent_sig = sig;
    sent_sender = sender_tid;
    return sent_count;
}

uint32_t __wrap_kernel_signal_send_thread(struct kernel_task *caller,
                                          kernel_pid_t tgid,
                                          kernel_pid_t tid,
                                          uint32_t sig,
                                          kernel_pid_t sender_tid)
{
    if (caller != (struct kernel_task *)(uintptr_t)1U) {
        return 0U;
    }
    sent_tgid = tgid;
    sent_tid = tid;
    sent_sig = sig;
    sent_sender = sender_tid;
    return sent_count;
}

enum kernel_task_status __wrap_kernel_task_tid(
    const struct kernel_task *task,
    kernel_pid_t *tid)
{
    if (task != (const struct kernel_task *)(uintptr_t)1U || tid == 0) {
        return KERNEL_TASK_STATUS_INVALID_ARGUMENT;
    }
    *tid = 7;
    return KERNEL_TASK_STATUS_OK;
}

static unsigned long result_changed(const struct kernel_syscall_result *result,
                                    enum kernel_syscall_action action,
                                    int64_t value)
{
    return result->action != action || result->value != value;
}

static unsigned long run_sigaction_cases(struct kernel_task *caller)
{
    struct kernel_syscall_request request = {
        .number = 134U,
        .arguments = {10U, UINT64_C(0x1000), UINT64_C(0x2000), 8U},
    };
    struct kernel_syscall_result result;
    unsigned long failures = 0U;

    memset(&copied_action, 0, sizeof(copied_action));
    if (kernel_syscall_dispatch(caller, &request, &result) !=
            KERNEL_SYSCALL_STATUS_OK ||
        result_changed(&result, KERNEL_SYSCALL_ACTION_RETURN, 0) ||
        memcmp(&copied_action, &old_action, sizeof(old_action)) != 0 ||
        set_sig != 10U ||
        memcmp(&set_action_value, &user_action, sizeof(user_action)) != 0) {
        failures++;
    }

    request.arguments[3] = 16U;
    if (kernel_syscall_dispatch(caller, &request, &result) !=
            KERNEL_SYSCALL_STATUS_OK ||
        result_changed(&result, KERNEL_SYSCALL_ACTION_RETURN, -KERNEL_EINVAL)) {
        failures++;
    }
    return failures;
}

static unsigned long run_sigmask_cases(struct kernel_task *caller)
{
    struct kernel_syscall_request request = {
        .number = 135U,
        .arguments = {LINUX_SIG_BLOCK, UINT64_C(0x3000), UINT64_C(0x4000), 8U},
    };
    struct kernel_syscall_result result;
    unsigned long failures = 0U;

    copied_mask = 0U;
    if (kernel_syscall_dispatch(caller, &request, &result) !=
            KERNEL_SYSCALL_STATUS_OK ||
        result_changed(&result, KERNEL_SYSCALL_ACTION_RETURN, 0) ||
        copied_mask != old_blocked || update_how != LINUX_SIG_BLOCK ||
        update_mask != user_mask) {
        failures++;
    }

    request.number = 136U;
    request.arguments[0] = UINT64_C(0x5000);
    request.arguments[1] = 8U;
    copied_pending = 0U;
    if (kernel_syscall_dispatch(caller, &request, &result) !=
            KERNEL_SYSCALL_STATUS_OK ||
        result_changed(&result, KERNEL_SYSCALL_ACTION_RETURN, 0) ||
        copied_pending != pending) {
        failures++;
    }
    return failures;
}

static unsigned long run_send_cases(struct kernel_task *caller)
{
    struct kernel_syscall_request request = {
        .number = 129U,
        .arguments = {42U, 10U},
    };
    struct kernel_syscall_result result;
    unsigned long failures = 0U;

    sent_count = 1U;
    if (kernel_syscall_dispatch(caller, &request, &result) !=
            KERNEL_SYSCALL_STATUS_OK ||
        result_changed(&result, KERNEL_SYSCALL_ACTION_RETURN, 0) ||
        sent_pid != 42 || sent_sig != 10U || sent_sender != 7) {
        failures++;
    }
    sent_count = 0U;
    if (kernel_syscall_dispatch(caller, &request, &result) !=
            KERNEL_SYSCALL_STATUS_OK ||
        result_changed(&result, KERNEL_SYSCALL_ACTION_RETURN, -KERNEL_ESRCH)) {
        failures++;
    }

    request.number = 130U;
    request.arguments[0] = 9U;
    sent_count = 1U;
    if (kernel_syscall_dispatch(caller, &request, &result) !=
            KERNEL_SYSCALL_STATUS_OK ||
        result_changed(&result, KERNEL_SYSCALL_ACTION_RETURN, 0) ||
        sent_tid != 9 || sent_tgid != 0) {
        failures++;
    }
    request.number = 131U;
    request.arguments[0] = 7U;
    request.arguments[1] = 9U;
    if (kernel_syscall_dispatch(caller, &request, &result) !=
            KERNEL_SYSCALL_STATUS_OK ||
        result_changed(&result, KERNEL_SYSCALL_ACTION_RETURN, 0) ||
        sent_tgid != 7 || sent_tid != 9) {
        failures++;
    }
    request.number = 139U;
    if (kernel_syscall_dispatch(caller, &request, &result) !=
            KERNEL_SYSCALL_STATUS_OK ||
        result_changed(&result, KERNEL_SYSCALL_ACTION_SIGNAL_RETURN, 0)) {
        failures++;
    }
    return failures;
}

unsigned long run_signal_cases(void)
{
    struct kernel_task *caller = (struct kernel_task *)(uintptr_t)1U;

    return run_sigaction_cases(caller) + run_sigmask_cases(caller) +
           run_send_cases(caller);
}
