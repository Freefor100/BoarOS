#include "pipe_internal.h"

#include <arch/riscv/context.h>
#include <kernel/errno.h>
#include <kernel/files.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
#include <kernel/physical_page.h>
#include <kernel/scheduler.h>
#include <kernel/signal.h>
#include <kernel/task.h>
#include <kernel/uaccess.h>

#include <stdint.h>
#include <string.h>

#define KERNEL_PIPE_ORDER 4U
#define KERNEL_PIPE_NO_BUFFER UINT64_MAX
#define KERNEL_PIPE_SIGPIPE 13U

static enum kernel_pipe_status pipe_destroy(struct kernel_pipe *pipe)
{
    if (pipe->buffer_physical != KERNEL_PIPE_NO_BUFFER) {
        if (physical_page_release_order(pipe->allocator,
                                        pipe->buffer_physical,
                                        KERNEL_PIPE_ORDER) !=
            PHYSICAL_PAGE_STATUS_OK) {
            return KERNEL_PIPE_STATUS_CLEANUP_REQUIRED;
        }
        pipe->buffer_physical = KERNEL_PIPE_NO_BUFFER;
        pipe->buffer = 0;
    }
    if (kernel_heap_release(pipe->heap, pipe) != KERNEL_HEAP_STATUS_OK) {
        return KERNEL_PIPE_STATUS_CLEANUP_REQUIRED;
    }
    return KERNEL_PIPE_STATUS_OK;
}

enum kernel_pipe_status kernel_pipe_create(
    struct kernel_heap *heap,
    struct kernel_pipe **owner)
{
    struct kernel_pipe *pipe;
    uint64_t physical_address;
    void *buffer;
    enum kernel_heap_status heap_status;
    enum physical_page_status page_status;

    if (heap == 0 || owner == 0 || *owner != 0) {
        return KERNEL_PIPE_STATUS_INVALID_ARGUMENT;
    }
    heap_status = kernel_heap_allocate_zeroed(heap,
                                              1U,
                                              sizeof(*pipe),
                                              (void **)&pipe);
    if (heap_status != KERNEL_HEAP_STATUS_OK) {
        return heap_status == KERNEL_HEAP_STATUS_EMPTY
                   ? KERNEL_PIPE_STATUS_NO_MEMORY
                   : KERNEL_PIPE_STATUS_STATE;
    }
    pipe->heap = heap;
    pipe->allocator = heap->page_allocator;
    pipe->buffer_physical = KERNEL_PIPE_NO_BUFFER;
    page_status = physical_page_allocate_order(heap->page_allocator,
                                               KERNEL_PIPE_ORDER,
                                               &physical_address);
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        if (pipe_destroy(pipe) != KERNEL_PIPE_STATUS_OK) {
            *owner = pipe;
            return KERNEL_PIPE_STATUS_CLEANUP_REQUIRED;
        }
        return page_status == PHYSICAL_PAGE_STATUS_EMPTY
                   ? KERNEL_PIPE_STATUS_NO_MEMORY
                   : KERNEL_PIPE_STATUS_STATE;
    }
    page_status = physical_page_resolve(heap->page_allocator,
                                        physical_address,
                                        &buffer);
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        pipe->buffer_physical = physical_address;
        if (pipe_destroy(pipe) != KERNEL_PIPE_STATUS_OK) {
            *owner = pipe;
            return KERNEL_PIPE_STATUS_CLEANUP_REQUIRED;
        }
        return KERNEL_PIPE_STATUS_STATE;
    }
    memset(buffer, 0, KERNEL_PIPE_CAPACITY);
    pipe->buffer_physical = physical_address;
    pipe->buffer = buffer;
    kernel_wait_queue_init(&pipe->read_queue);
    kernel_wait_queue_init(&pipe->write_queue);
    *owner = pipe;
    return KERNEL_PIPE_STATUS_OK;
}

enum kernel_pipe_status kernel_pipe_acquire_endpoint(
    struct kernel_pipe *pipe,
    uint32_t endpoint)
{
    if (pipe == 0 || pipe->heap == 0 || pipe->buffer == 0 ||
        pipe->destroy_pending != 0U ||
        (endpoint != KERNEL_PIPE_ENDPOINT_READ &&
         endpoint != KERNEL_PIPE_ENDPOINT_WRITE)) {
        return KERNEL_PIPE_STATUS_INVALID_ARGUMENT;
    }
    if (endpoint == KERNEL_PIPE_ENDPOINT_READ) {
        if (pipe->readers == UINT32_MAX) {
            return KERNEL_PIPE_STATUS_STATE;
        }
        pipe->readers++;
    } else {
        if (pipe->writers == UINT32_MAX) {
            return KERNEL_PIPE_STATUS_STATE;
        }
        pipe->writers++;
    }
    return KERNEL_PIPE_STATUS_OK;
}

enum kernel_pipe_status kernel_pipe_destroy_unowned(
    struct kernel_pipe *pipe)
{
    uintptr_t saved;
    enum kernel_pipe_status status;

    if (pipe == 0 || pipe->heap == 0 || pipe->readers != 0U ||
        pipe->writers != 0U) {
        return KERNEL_PIPE_STATUS_INVALID_ARGUMENT;
    }
    saved = riscv_interrupt_save();
    pipe->destroy_pending = 1U;
    status = pipe_destroy(pipe);
    riscv_interrupt_restore(saved);
    return status;
}

enum kernel_pipe_status kernel_pipe_release_endpoint(
    struct kernel_pipe *pipe,
    uint32_t endpoint)
{
    uintptr_t saved;
    enum kernel_pipe_status status;

    if (pipe == 0 || pipe->heap == 0 ||
        (endpoint != KERNEL_PIPE_ENDPOINT_READ &&
         endpoint != KERNEL_PIPE_ENDPOINT_WRITE)) {
        return KERNEL_PIPE_STATUS_INVALID_ARGUMENT;
    }
    saved = riscv_interrupt_save();
    if (pipe->destroy_pending != 0U) {
        status = pipe_destroy(pipe);
        riscv_interrupt_restore(saved);
        return status;
    }
    if (endpoint == KERNEL_PIPE_ENDPOINT_READ) {
        if (pipe->readers == 0U) {
            riscv_interrupt_restore(saved);
            return KERNEL_PIPE_STATUS_STATE;
        }
        pipe->readers--;
    } else {
        if (pipe->writers == 0U) {
            riscv_interrupt_restore(saved);
            return KERNEL_PIPE_STATUS_STATE;
        }
        pipe->writers--;
    }
    if (pipe->writers == 0U) {
        (void)kernel_wait_queue_wake_all(&pipe->read_queue);
    }
    if (pipe->readers == 0U) {
        (void)kernel_wait_queue_wake_all(&pipe->write_queue);
    }
    if (pipe->readers == 0U && pipe->writers == 0U) {
        pipe->destroy_pending = 1U;
        status = pipe_destroy(pipe);
    } else {
        status = KERNEL_PIPE_STATUS_OK;
    }
    riscv_interrupt_restore(saved);
    return status;
}

static uint64_t pipe_remaining(uint64_t position)
{
    return KERNEL_PIPE_CAPACITY - position;
}

static void pipe_consume(struct kernel_pipe *pipe, uint64_t count)
{
    pipe->read_position =
        (pipe->read_position + count) % KERNEL_PIPE_CAPACITY;
    pipe->bytes -= count;
}

static void pipe_produce(struct kernel_pipe *pipe, uint64_t count)
{
    pipe->write_position =
        (pipe->write_position + count) % KERNEL_PIPE_CAPACITY;
    pipe->bytes += count;
}

enum kernel_pipe_status kernel_pipe_read(
    struct kernel_pipe *pipe,
    struct kernel_mm *mm,
    uint64_t user_buffer,
    uint64_t count,
    uint32_t open_flags,
    int64_t *linux_result)
{
    uintptr_t saved;
    enum kernel_scheduler_status scheduler_status;
    enum kernel_wait_wake_reason wake_reason = KERNEL_WAIT_WOKEN;
    enum kernel_uaccess_status access_status;
    uint64_t requested;
    size_t copied;

    if (pipe == 0 || mm == 0 || linux_result == 0 ||
        pipe->heap == 0 || pipe->buffer == 0) {
        return KERNEL_PIPE_STATUS_INVALID_ARGUMENT;
    }
    if ((open_flags & 3U) == 1U) {
        *linux_result = -KERNEL_EBADF;
        return KERNEL_PIPE_STATUS_OK;
    }
    if (kernel_user_range_check(user_buffer, (size_t)count) !=
        KERNEL_UACCESS_STATUS_OK) {
        *linux_result = -KERNEL_EFAULT;
        return KERNEL_PIPE_STATUS_OK;
    }
    if (count == 0U) {
        *linux_result = 0;
        return KERNEL_PIPE_STATUS_OK;
    }
    saved = riscv_interrupt_save();
    requested = count < pipe->bytes ? count : pipe->bytes;
    while (requested == 0U) {
        if (pipe->writers == 0U) {
            riscv_interrupt_restore(saved);
            *linux_result = 0;
            return KERNEL_PIPE_STATUS_OK;
        }
        if ((open_flags & KERNEL_PIPE_NONBLOCK) != 0U) {
            riscv_interrupt_restore(saved);
            *linux_result = -KERNEL_EAGAIN;
            return KERNEL_PIPE_STATUS_OK;
        }
        scheduler_status = kernel_scheduler_block_current(&pipe->read_queue,
                                                           0U,
                                                           1,
                                                           &wake_reason);
        if (scheduler_status != KERNEL_SCHEDULER_STATUS_OK) {
            riscv_interrupt_restore(saved);
            return KERNEL_PIPE_STATUS_STATE;
        }
        if (wake_reason == KERNEL_WAIT_SIGNALLED) {
            kernel_signal_note_syscall_restart(kernel_task_current());
            riscv_interrupt_restore(saved);
            *linux_result = -KERNEL_ERESTARTSYS;
            return KERNEL_PIPE_STATUS_OK;
        }
        requested = count < pipe->bytes ? count : pipe->bytes;
    }

    copied = 0U;
    access_status = KERNEL_UACCESS_STATUS_OK;
    while ((uint64_t)copied < requested) {
        uint64_t chunk64 = requested - (uint64_t)copied;
        size_t chunk;
        size_t part_copied = 0U;
        enum kernel_uaccess_status part_status;

        if (chunk64 > pipe_remaining(pipe->read_position)) {
            chunk64 = pipe_remaining(pipe->read_position);
        }
        chunk = (size_t)chunk64;
        part_status = kernel_copy_to_user(
            mm,
            user_buffer + copied,
            pipe->buffer + pipe->read_position,
            chunk,
            &part_copied);
        pipe_consume(pipe, part_copied);
        copied += part_copied;
        if (part_status != KERNEL_UACCESS_STATUS_OK ||
            part_copied != chunk) {
            access_status = part_status;
            break;
        }
    }
    if (copied != 0U) {
        (void)kernel_wait_queue_wake_all(&pipe->write_queue);
    }
    riscv_interrupt_restore(saved);
    if (copied == 0U && access_status == KERNEL_UACCESS_STATUS_FAULT) {
        *linux_result = -KERNEL_EFAULT;
    } else if (copied == 0U) {
        *linux_result = -KERNEL_EIO;
    } else {
        *linux_result = (int64_t)copied;
    }
    return KERNEL_PIPE_STATUS_OK;
}

static enum kernel_pipe_status pipe_signal_broken(
    struct kernel_pipe *pipe,
    int64_t *linux_result)
{
    if (kernel_signal_send_task(kernel_task_current(),
                           KERNEL_PIPE_SIGPIPE,
                           0) != KERNEL_SIGNAL_STATUS_OK) {
        return KERNEL_PIPE_STATUS_STATE;
    }
    *linux_result = -KERNEL_EPIPE;
    (void)pipe;
    return KERNEL_PIPE_STATUS_OK;
}

enum kernel_pipe_status kernel_pipe_writev(
    struct kernel_pipe *pipe,
    struct kernel_mm *mm,
    const struct kernel_uaccess_iovec *iov,
    size_t iov_count,
    uint64_t count,
    uint32_t open_flags,
    int64_t *linux_result)
{
    uintptr_t saved;
    enum kernel_scheduler_status scheduler_status;
    enum kernel_wait_wake_reason wake_reason = KERNEL_WAIT_WOKEN;
    uint64_t total = 0U;
    int atomic;
    size_t iov_index = 0U;
    uint64_t iov_offset = 0U;

    if (pipe == 0 || mm == 0 || linux_result == 0 ||
        pipe->heap == 0 || pipe->buffer == 0) {
        return KERNEL_PIPE_STATUS_INVALID_ARGUMENT;
    }
    if ((open_flags & 3U) == 0U) {
        *linux_result = -KERNEL_EBADF;
        return KERNEL_PIPE_STATUS_OK;
    }
    if (count == 0U) {
        *linux_result = 0;
        return KERNEL_PIPE_STATUS_OK;
    }
    atomic = count <= KERNEL_PIPE_ATOMIC_WRITE;
    saved = riscv_interrupt_save();
    while (total < count) {
        uint64_t free_bytes = KERNEL_PIPE_CAPACITY - pipe->bytes;
        uint64_t request = count - total;
        uint64_t chunk64;
        size_t copied;
        size_t chunk;
        enum kernel_uaccess_status access_status;

        if (pipe->readers == 0U) {
            enum kernel_pipe_status status = pipe_signal_broken(pipe,
                                                                 linux_result);

            if (total != 0U && status == KERNEL_PIPE_STATUS_OK) {
                *linux_result = (int64_t)total;
            }
            riscv_interrupt_restore(saved);
            return status;
        }
        if (free_bytes == 0U || (atomic != 0 && free_bytes < request)) {
            if ((open_flags & KERNEL_PIPE_NONBLOCK) != 0U) {
                riscv_interrupt_restore(saved);
                *linux_result = total != 0U ? (int64_t)total : -KERNEL_EAGAIN;
                return KERNEL_PIPE_STATUS_OK;
            }
            scheduler_status = kernel_scheduler_block_current(
                &pipe->write_queue,
                0U,
                1,
                &wake_reason);
            if (scheduler_status != KERNEL_SCHEDULER_STATUS_OK) {
                riscv_interrupt_restore(saved);
                return KERNEL_PIPE_STATUS_STATE;
            }
            if (wake_reason == KERNEL_WAIT_SIGNALLED) {
                if (total == 0U) {
                    kernel_signal_note_syscall_restart(kernel_task_current());
                    *linux_result = -KERNEL_ERESTARTSYS;
                } else {
                    *linux_result = (int64_t)total;
                }
                riscv_interrupt_restore(saved);
                return KERNEL_PIPE_STATUS_OK;
            }
            continue;
        }
        chunk64 = request < free_bytes ? request : free_bytes;
        if (chunk64 > pipe_remaining(pipe->write_position)) {
            chunk64 = pipe_remaining(pipe->write_position);
        }
        while (iov_index < iov_count && iov_offset == iov[iov_index].length) {
            iov_index++;
            iov_offset = 0U;
        }
        if (iov_index == iov_count) {
            riscv_interrupt_restore(saved);
            return KERNEL_PIPE_STATUS_STATE;
        }
        if (chunk64 > iov[iov_index].length - iov_offset) {
            chunk64 = iov[iov_index].length - iov_offset;
        }
        chunk = (size_t)chunk64;
        copied = 0U;
        access_status = kernel_copy_from_user(
            mm,
            pipe->buffer + pipe->write_position,
            iov[iov_index].base + iov_offset,
            chunk,
            &copied);
        pipe_produce(pipe, copied);
        total += copied;
        iov_offset += copied;
        /* Empty -> readable makes every sleeping reader eligible. Further
         * iovecs before a schedule need no repeated blocked-list scan. */
        if (copied != 0U && pipe->bytes == copied) {
            (void)kernel_wait_queue_wake_all(&pipe->read_queue);
        }
        if (access_status != KERNEL_UACCESS_STATUS_OK || copied != chunk) {
            riscv_interrupt_restore(saved);
            *linux_result = total != 0U ? (int64_t)total : -KERNEL_EFAULT;
            return KERNEL_PIPE_STATUS_OK;
        }
    }
    riscv_interrupt_restore(saved);
    *linux_result = (int64_t)total;
    return KERNEL_PIPE_STATUS_OK;
}
