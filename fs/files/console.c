#include "private.h"
#include "../uaccess_iov_internal.h"

#include <arch/riscv/context.h>
#include <arch/riscv/virt_uart.h>
#include <kernel/errno.h>
#include <kernel/open_file.h>
#include <kernel/scheduler.h>
#include <kernel/signal.h>
#include <kernel/task.h>
#include <kernel/uaccess.h>

#include <stddef.h>
#include <stdint.h>

#define KERNEL_FILES_CONSOLE_STAGING 64U

static struct kernel_wait_queue console_input_queue;

void kernel_console_poll_input(void)
{
    if (console_input_queue.initialized == KERNEL_WAIT_QUEUE_INITIALIZED &&
        virt_uart_rx_ready() != 0U) {
        (void)kernel_wait_queue_wake_all(&console_input_queue);
    }
}

uint32_t kernel_console_poll(uint32_t requested_events,
                             struct kernel_wait_queue **out_queue)
{
    uint32_t events = KERNEL_POLLOUT | KERNEL_POLLWRNORM;

    if (console_input_queue.initialized != KERNEL_WAIT_QUEUE_INITIALIZED) {
        kernel_wait_queue_init(&console_input_queue);
    }

    if (virt_uart_rx_ready() != 0U) {
        events |= (KERNEL_POLLIN | KERNEL_POLLRDNORM);
    }

    if (out_queue != 0) {
        if ((requested_events & (KERNEL_POLLIN | KERNEL_POLLRDNORM)) != 0U ||
            requested_events == 0U) {
            *out_queue = &console_input_queue;
        } else {
            *out_queue = 0;
        }
    }

    return events;
}

enum kernel_files_status kernel_files_read_console(
    struct kernel_files *files,
    struct kernel_mm *mm,
    const struct kernel_uaccess_iovec *iov,
    size_t iov_count,
    uint64_t count,
    int64_t *linux_result)
{
    struct kernel_uaccess_iov_cursor cursor = {iov, iov_count, 0U, 0U};
    unsigned char staging[KERNEL_FILES_CONSOLE_STAGING];
    enum kernel_wait_wake_reason wake_reason = KERNEL_WAIT_WOKEN;
    enum kernel_uaccess_status access_status;
    enum kernel_scheduler_status sleep_status;
    size_t staged = 0;
    size_t copied = 0;
    uintptr_t saved;

    for (size_t index = 0U; index < iov_count; index++) {
        if (kernel_user_range_check(iov[index].base,
                                    (size_t)iov[index].length) !=
            KERNEL_UACCESS_STATUS_OK) {
            files->record->statistics.read_failures++;
            *linux_result = -KERNEL_EFAULT;
            return KERNEL_FILES_STATUS_OK;
        }
    }
    if (count == 0U) {
        *linux_result = 0;
        return KERNEL_FILES_STATUS_OK;
    }

    saved = riscv_interrupt_save();
    if (console_input_queue.initialized != KERNEL_WAIT_QUEUE_INITIALIZED) {
        kernel_wait_queue_init(&console_input_queue);
    }
    for (;;) {
        while (staged < KERNEL_FILES_CONSOLE_STAGING && staged < count &&
               virt_uart_rx_ready() != 0U) {
            staging[staged] = (unsigned char)virt_uart_getc();
            staged++;
        }
        if (staged != 0U) {
            break;
        }
        sleep_status = kernel_scheduler_block_current(&console_input_queue,
                                                      0U,
                                                      1,
                                                      &wake_reason);
        if (sleep_status != KERNEL_SCHEDULER_STATUS_OK) {
            riscv_interrupt_restore(saved);
            return KERNEL_FILES_STATUS_STATE;
        }
        if (wake_reason == KERNEL_WAIT_SIGNALLED) {
            kernel_signal_note_syscall_restart(kernel_task_current());
            riscv_interrupt_restore(saved);
            *linux_result = -KERNEL_ERESTARTSYS;
            return KERNEL_FILES_STATUS_OK;
        }
    }
    riscv_interrupt_restore(saved);

    access_status = kernel_copy_to_user_iov(mm, &cursor, staging,
                                            staged, &copied);
    if (access_status != KERNEL_UACCESS_STATUS_OK &&
        access_status != KERNEL_UACCESS_STATUS_FAULT) {
        return KERNEL_FILES_STATUS_STATE;
    }
    if (access_status == KERNEL_UACCESS_STATUS_FAULT && copied == 0U) {
        files->record->statistics.read_failures++;
        *linux_result = -KERNEL_EFAULT;
        return KERNEL_FILES_STATUS_OK;
    }

    *linux_result = (int64_t)copied;
    return KERNEL_FILES_STATUS_OK;
}
