#include "private.h"
#include <kernel/errno.h>
#include <kernel/random.h>
#include <kernel/signal.h>
#include <kernel/task.h>
#include <kernel/uaccess.h>
#include <stdint.h>

enum kernel_syscall_status syscall_handle_getrandom(
    struct kernel_task *caller, const struct kernel_syscall_request *request,
    struct kernel_syscall_result *decoded)
{
    uint64_t address = request->arguments[0];
    size_t size = request->arguments[1], total = 0;
    unsigned flags = (unsigned)request->arguments[2];
    struct kernel_mm *mm;
    uint8_t block[256];
    decoded->action = KERNEL_SYSCALL_ACTION_RETURN;
    decoded->value = -KERNEL_EINVAL;
    if ((flags & ~7U) || (flags & 6U) == 6U) return KERNEL_SYSCALL_STATUS_OK;
    if (!(flags & 4U)) {
        int result = kernel_random_wait_ready(flags & 1U);
        if (result) {
            if (result == -KERNEL_EINTR) {
                kernel_signal_note_syscall_restart(caller);
                result = -KERNEL_ERESTARTSYS;
            }
            decoded->value = result; return KERNEL_SYSCALL_STATUS_OK;
        }
    }
    if (size > 0x7ffff000U) size = 0x7ffff000U;
    if (kernel_user_range_check(address, size) != KERNEL_UACCESS_STATUS_OK ||
        (!size && address && kernel_user_range_check(address - 1U, 1U) !=
                                  KERNEL_UACCESS_STATUS_OK)) {
        decoded->value = -KERNEL_EFAULT; return KERNEL_SYSCALL_STATUS_OK;
    }
    if (kernel_task_mm_borrow_mutable(caller, &mm) != KERNEL_TASK_STATUS_OK)
        return KERNEL_SYSCALL_STATUS_INVALID_ARGUMENT;
    while (total < size) {
        size_t n = size - total > sizeof(block) ? sizeof(block) : size - total;
        size_t copied = 0;
        if (total && kernel_signal_has_pending(caller)) break;
        if (kernel_random_fill(block, n) != KERNEL_RANDOM_STATUS_OK) {
            kernel_random_erase(block, sizeof(block));
            decoded->value = total ? (int64_t)total : -KERNEL_EIO;
            return KERNEL_SYSCALL_STATUS_OK;
        }
        enum kernel_uaccess_status status = kernel_copy_to_user(
            mm, address + total, block, n, &copied);
        kernel_random_erase(block, sizeof(block));
        total += copied;
        if (status != KERNEL_UACCESS_STATUS_OK || copied != n) {
            decoded->value = total ? (int64_t)total : -KERNEL_EFAULT;
            return KERNEL_SYSCALL_STATUS_OK;
        }
    }
    decoded->value = (int64_t)total;
    return KERNEL_SYSCALL_STATUS_OK;
}
