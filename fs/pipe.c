#include "pipe_internal.h"
#include "uaccess_iov_internal.h"

#include <arch/riscv/context.h>
#include <kernel/errno.h>
#include <kernel/files.h>
#include <kernel/heap.h>
#include <kernel/mm.h>
#include <kernel/open_file.h>
#include <kernel/page.h>
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
        (void)physical_page_release_order(pipe->allocator,
                                        pipe->buffer_physical,
                                        KERNEL_PIPE_ORDER);
        pipe->buffer_physical = KERNEL_PIPE_NO_BUFFER;
        pipe->buffer = 0;
    }
    (void)kernel_heap_release(pipe->heap, pipe);
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
        (void)pipe_destroy(pipe);
        return page_status == PHYSICAL_PAGE_STATUS_EMPTY
                   ? KERNEL_PIPE_STATUS_NO_MEMORY
                   : KERNEL_PIPE_STATUS_STATE;
    }
    page_status = physical_page_resolve(heap->page_allocator,
                                        physical_address,
                                        &buffer);
    if (page_status != PHYSICAL_PAGE_STATUS_OK) {
        pipe->buffer_physical = physical_address;
        (void)pipe_destroy(pipe);
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
        status = pipe_destroy(pipe);
    } else {
        status = KERNEL_PIPE_STATUS_OK;
    }
    riscv_interrupt_restore(saved);
    return status;
}

static uint16_t next_slot(uint16_t slot)
{
    return (uint16_t)((slot + 1U) %
                      (KERNEL_PIPE_CAPACITY / BOAROS_PAGE_SIZE));
}

static void pipe_consume(struct kernel_pipe *pipe, uint64_t count)
{
    uint16_t slot = pipe->head;

    pipe->page_offset[slot] += (uint16_t)count;
    pipe->page_length[slot] -= (uint16_t)count;
    pipe->bytes -= count;
    if (pipe->page_length[slot] == 0U) {
        pipe->page_offset[slot] = 0U;
        pipe->head = next_slot(slot);
        pipe->slots--;
    }
}

static uint16_t pipe_tail_space(const struct kernel_pipe *pipe)
{
    uint16_t last;

    if (pipe->slots == 0U) return 0U;
    last = (uint16_t)((pipe->tail +
                       KERNEL_PIPE_CAPACITY / BOAROS_PAGE_SIZE - 1U) %
                      (KERNEL_PIPE_CAPACITY / BOAROS_PAGE_SIZE));
    return (uint16_t)(BOAROS_PAGE_SIZE - pipe->page_offset[last] -
                      pipe->page_length[last]);
}

enum kernel_pipe_status kernel_pipe_readv(
    struct kernel_pipe *pipe,
    struct kernel_mm *mm,
    const struct kernel_uaccess_iovec *iov,
    size_t iov_count,
    uint64_t count,
    uint32_t open_flags,
    int64_t *linux_result)
{
    struct kernel_uaccess_iov_cursor cursor = {iov, iov_count, 0U, 0U};
    uintptr_t saved;
    enum kernel_scheduler_status scheduler_status;
    enum kernel_wait_wake_reason wake_reason = KERNEL_WAIT_WOKEN;
    uint64_t requested;
    size_t committed;
    int fault = 0;

    if (pipe == 0 || mm == 0 || linux_result == 0 ||
        pipe->heap == 0 || pipe->buffer == 0) {
        return KERNEL_PIPE_STATUS_INVALID_ARGUMENT;
    }
    if ((open_flags & 3U) == 1U) {
        *linux_result = -KERNEL_EBADF;
        return KERNEL_PIPE_STATUS_OK;
    }
    for (size_t index = 0U; index < iov_count; index++) {
        if (kernel_user_range_check(iov[index].base,
                                    (size_t)iov[index].length) !=
            KERNEL_UACCESS_STATUS_OK) {
            *linux_result = -KERNEL_EFAULT;
            return KERNEL_PIPE_STATUS_OK;
        }
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

    committed = 0U;
    while ((uint64_t)committed < requested) {
        uint16_t slot = pipe->head;
        uint64_t chunk64 = requested - (uint64_t)committed;
        size_t chunk;
        size_t part_copied = 0U;
        enum kernel_uaccess_status part_status;

        if (pipe->slots == 0U || pipe->page_length[slot] == 0U) {
            riscv_interrupt_restore(saved);
            return KERNEL_PIPE_STATUS_STATE;
        }
        if (chunk64 > pipe->page_length[slot]) {
            chunk64 = pipe->page_length[slot];
        }
        chunk = (size_t)chunk64;
        part_status = kernel_copy_to_user_iov(
            mm, &cursor,
            pipe->buffer + (size_t)slot * BOAROS_PAGE_SIZE +
                pipe->page_offset[slot],
            chunk,
            &part_copied);
        if (part_status != KERNEL_UACCESS_STATUS_OK ||
            part_copied != chunk) {
            if (part_status != KERNEL_UACCESS_STATUS_FAULT) {
                riscv_interrupt_restore(saved);
                return KERNEL_PIPE_STATUS_STATE;
            }
            fault = 1;
            break;
        }
        pipe_consume(pipe, chunk);
        committed += chunk;
    }
    if (committed != 0U) {
        (void)kernel_wait_queue_wake_all(&pipe->write_queue);
    }
    riscv_interrupt_restore(saved);
    if (committed == 0U && fault) {
        *linux_result = -KERNEL_EFAULT;
    } else {
        *linux_result = (int64_t)committed;
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
    uint16_t merge_bytes = (uint16_t)(count & BOAROS_PAGE_MASK);
    struct kernel_uaccess_iov_cursor cursor = {iov, iov_count, 0U, 0U};

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
    saved = riscv_interrupt_save();
    while (total < count) {
        uint64_t free_slots = KERNEL_PIPE_CAPACITY / BOAROS_PAGE_SIZE -
                              pipe->slots;
        uint64_t request = count - total;
        uint64_t chunk64;
        uint16_t tail_space = pipe_tail_space(pipe);
        uint16_t slot;
        int merging = total == 0U && merge_bytes != 0U &&
                      pipe->slots != 0U && tail_space >= merge_bytes;
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
        if (!merging && free_slots == 0U) {
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
        /* Linux merges only the request's page remainder into the previous
         * fragment, then publishes fresh page-sized fragments. The boundary
         * determines what a later faulting read may consume. */
        if (!merging) {
            slot = pipe->tail;
            chunk64 = request < BOAROS_PAGE_SIZE
                          ? request : BOAROS_PAGE_SIZE;
        } else {
            slot = (uint16_t)((pipe->tail +
                       KERNEL_PIPE_CAPACITY / BOAROS_PAGE_SIZE - 1U) %
                       (KERNEL_PIPE_CAPACITY / BOAROS_PAGE_SIZE));
            chunk64 = merge_bytes;
        }
        chunk = (size_t)chunk64;
        copied = 0U;
        access_status = kernel_copy_from_user_iov(
            mm,
            &cursor,
            pipe->buffer + (size_t)slot * BOAROS_PAGE_SIZE +
                (!merging ? 0U : pipe->page_offset[slot] +
                                   pipe->page_length[slot]),
            chunk,
            &copied);
        if (access_status != KERNEL_UACCESS_STATUS_OK || copied != chunk) {
            riscv_interrupt_restore(saved);
            if (access_status != KERNEL_UACCESS_STATUS_FAULT) {
                return KERNEL_PIPE_STATUS_STATE;
            }
            *linux_result = total != 0U ? (int64_t)total : -KERNEL_EFAULT;
            return KERNEL_PIPE_STATUS_OK;
        }
        if (!merging) {
            pipe->page_offset[slot] = 0U;
            pipe->page_length[slot] = 0U;
            pipe->tail = next_slot(slot);
            pipe->slots++;
        }
        pipe->page_length[slot] += (uint16_t)chunk;
        pipe->bytes += chunk;
        total += chunk;
        /* Any bytes produced make sleeping readers or epoll watchers eligible. */
        (void)kernel_wait_queue_wake_all(&pipe->read_queue);
    }
    riscv_interrupt_restore(saved);
    *linux_result = (int64_t)total;
    return KERNEL_PIPE_STATUS_OK;
}

uint32_t kernel_pipe_poll(
    struct kernel_pipe *pipe,
    uint32_t endpoint,
    struct kernel_wait_queue **out_queue)
{
    uint32_t events = 0U;

    if (pipe == 0) {
        if (out_queue != 0) *out_queue = 0;
        return KERNEL_POLLNVAL;
    }

    if (endpoint == KERNEL_PIPE_ENDPOINT_READ) {
        if (out_queue != 0) {
            *out_queue = &pipe->read_queue;
        }
        if (pipe->bytes > 0U) {
            events |= (KERNEL_POLLIN | KERNEL_POLLRDNORM);
        }
        if (pipe->writers == 0U) {
            events |= KERNEL_POLLHUP;
        }
    } else if (endpoint == KERNEL_PIPE_ENDPOINT_WRITE) {
        if (out_queue != 0) {
            *out_queue = &pipe->write_queue;
        }
        if (pipe->readers == 0U) {
            events |= KERNEL_POLLERR;
            events |= (KERNEL_POLLOUT | KERNEL_POLLWRNORM);
        } else if (pipe->slots < KERNEL_PIPE_CAPACITY / BOAROS_PAGE_SIZE) {
            events |= (KERNEL_POLLOUT | KERNEL_POLLWRNORM);
        }
    } else {
        if (out_queue != 0) *out_queue = 0;
        return KERNEL_POLLNVAL;
    }

    return events;
}
