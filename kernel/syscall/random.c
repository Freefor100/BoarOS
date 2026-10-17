#include <kernel/scheduler.h>
#include <kernel/cpu.h>
#include <kernel/page.h>
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
    KERNEL_SERVICE_SCOPE(service);
    uint64_t address = request->arguments[0];
    size_t size = request->arguments[1], total = 0;
    unsigned flags = (unsigned)request->arguments[2];
    struct kernel_mm *mm;
    uint8_t small[256], *block = small;
    size_t capacity = sizeof(small);
    struct kernel_task_io_buffer workspace = {0};
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
    if (size > capacity) {
        if (kernel_task_io_buffer_acquire(&workspace, mm->allocator) != KERNEL_TASK_STATUS_OK) {
            decoded->value = -KERNEL_ENOMEM;
            return KERNEL_SYSCALL_STATUS_OK;
        }
        block = workspace.data;
        capacity = BOAROS_PAGE_SIZE;
    }
    /* 随机池自己保护状态；复制只借用已固定的用户页，无业务 raw 锁。
     * 长请求在这个边界允许 tick 和信号推进，退出恢复调用者 IRQ 状态。 */
    kernel_assert_can_block();
    uintptr_t saved_interrupts = arch_interrupt_save();
    arch_interrupt_restore(ARCH_INTERRUPT_ENABLE_MASK);
    int64_t error = 0;
    while (total < size) {
        size_t n = size - total > capacity ? capacity : size - total;
        size_t page_remaining = BOAROS_PAGE_SIZE - ((address + total) & BOAROS_PAGE_MASK);
        if (workspace.allocator && n > page_remaining) n = page_remaining;
        size_t copied = 0;
        if (total && kernel_signal_has_pending(caller)) break;
        if (kernel_random_fill(block, n) != KERNEL_RANDOM_STATUS_OK) {
            error = -KERNEL_EIO;
            break;
        }
        enum kernel_uaccess_status status = kernel_copy_to_user(
            mm, address + total, block, n, &copied);
        total += copied;
        if (status != KERNEL_UACCESS_STATUS_OK || copied != n) {
            error = -KERNEL_EFAULT;
            break;
        }
    }
    /* 每次覆盖同一工作区，中间不用反复清零；最后一次实际使用结束再擦除。 */
    kernel_random_erase(block, size < capacity ? size : capacity);
    if (workspace.allocator) kernel_task_io_buffer_release(&workspace);
    arch_interrupt_restore(saved_interrupts);
    decoded->value = total ? (int64_t)total : error;
    return KERNEL_SYSCALL_STATUS_OK;
}
