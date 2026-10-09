#include <arch/context.h>
#include <arch/platform_io.h>
#include <kernel/errno.h>
#include <kernel/files.h>
#include <kernel/open_file.h>
#include <kernel/scheduler.h>
#include <kernel/signal.h>
#include <kernel/task.h>

#include <stddef.h>
#include <stdint.h>

#define KERNEL_FILES_CONSOLE_STAGING 64U

static struct kernel_wait_queue console_input_queue;

void kernel_console_poll_input(void)
{
    if (console_input_queue.initialized == KERNEL_WAIT_QUEUE_INITIALIZED &&
        arch_uart_rx_ready() != 0U) {
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

    if (arch_uart_rx_ready() != 0U) {
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

int kernel_console_read_buffer(uint32_t flags, void *buffer, size_t size,
                               size_t *bytes_read)
{
    enum kernel_wait_wake_reason wake_reason = KERNEL_WAIT_WOKEN;
    enum kernel_scheduler_status sleep_status;
    unsigned char *staging = buffer;
    size_t staged = 0U;
    uintptr_t saved;

    if (!buffer || !bytes_read) return -KERNEL_EINVAL;
    *bytes_read = 0U;
    if (size == 0U) return 0;
    if (size > KERNEL_FILES_CONSOLE_STAGING) size = KERNEL_FILES_CONSOLE_STAGING;
    saved = arch_interrupt_save();
    if (console_input_queue.initialized != KERNEL_WAIT_QUEUE_INITIALIZED)
        kernel_wait_queue_init(&console_input_queue);
    for (;;) {
        while (staged < size && arch_uart_rx_ready() != 0U)
            staging[staged++] = (unsigned char)arch_uart_getc();
        if (staged) break;
        if (flags & KERNEL_FILES_O_NONBLOCK) {
            arch_interrupt_restore(saved);
            return -KERNEL_EAGAIN;
        }
        sleep_status = KERNEL_WAIT_RECHECK(&console_input_queue,
                                                      0U, 1, &wake_reason,
                (!arch_uart_rx_ready()));
        if (sleep_status != KERNEL_SCHEDULER_STATUS_OK) {
            arch_interrupt_restore(saved);
            return -KERNEL_EIO;
        }
        if (wake_reason == KERNEL_WAIT_SIGNALLED) {
            kernel_signal_note_syscall_restart(kernel_task_current());
            arch_interrupt_restore(saved);
            return -KERNEL_ERESTARTSYS;
        }
    }
    arch_interrupt_restore(saved);
    *bytes_read = staged;
    return 0;
}
