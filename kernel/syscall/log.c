#include "private.h"
#include <kernel/log.h>
#include <kernel/errno.h>
#include <kernel/fs_context.h>
#include <kernel/heap.h>
#include <kernel/signal.h>
#include <kernel/task.h>
#include <kernel/uaccess.h>

struct log_user_copy { struct kernel_mm *mm; uint64_t address; };
static int copy_log(void *context, size_t offset, const char *data, size_t length)
{
    struct log_user_copy *target = context;
    size_t copied = 0;
    return kernel_copy_to_user(target->mm, target->address + offset, data,
                               length, &copied) != KERNEL_UACCESS_STATUS_OK || copied != length;
}
enum kernel_syscall_status syscall_handle_syslog(
    struct kernel_task *caller, const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    int action = (int)request->arguments[0], length = (int)request->arguments[2];
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    /* All current tasks have the immutable root identity. The policy itself
     * is centralized in the log layer for a future credential owner. */
    if (action < 2 || action > 4 || length < 0 || !request->arguments[1]) {
        decoded->value = kernel_log_action(action, length, 1, NULL, NULL, NULL);
        return KERNEL_SYSCALL_STATUS_OK;
    }
    if (kernel_user_range_check(request->arguments[1], (size_t)length) != KERNEL_UACCESS_STATUS_OK) {
        decoded->value = -KERNEL_EFAULT; return KERNEL_SYSCALL_STATUS_OK;
    }
    if (!length) { decoded->value = 0; return KERNEL_SYSCALL_STATUS_OK; }
    struct log_user_copy target = {.address = request->arguments[1]};
    const struct kernel_fs_context *fs;
    if (kernel_task_mm_borrow_mutable(caller, &target.mm) != KERNEL_TASK_STATUS_OK ||
        kernel_task_fs_context_borrow(caller, &fs) != KERNEL_TASK_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    char *scratch;
    if (kernel_heap_allocate(fs->heap, KERNEL_LOG_RECORD_MAX, (void **)&scratch) != KERNEL_HEAP_STATUS_OK) {
        decoded->value = -KERNEL_ENOMEM; return KERNEL_SYSCALL_STATUS_OK;
    }
    decoded->value = kernel_log_action(action, length, 1, copy_log, &target, scratch);
    if (kernel_heap_release(fs->heap, scratch) != KERNEL_HEAP_STATUS_OK) __builtin_trap();
    if (decoded->value == -KERNEL_EINTR) {
        kernel_signal_note_syscall_restart(caller); decoded->value = -KERNEL_ERESTARTSYS;
    }
    return KERNEL_SYSCALL_STATUS_OK;
}
