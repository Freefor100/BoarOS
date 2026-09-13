#include "epoll_internal.h"
#include "private.h"
#include "../open_file_internal.h"

#include <arch/riscv/context.h>
#include <kernel/errno.h>
#include <kernel/heap.h>
#include <kernel/open_file.h>
#include <kernel/scheduler.h>
#include <kernel/signal.h>
#include <kernel/time.h>
#include <kernel/uaccess.h>
#include <kernel/files.h>

#include <stddef.h>
#include <stdint.h>

#define KERNEL_EPOLL_STACK_CAPACITY 8U
#define KERNEL_EPOLL_MAX_EVENTS 1024U

static void kernel_epoll_wait_callback(
    struct kernel_wait_node *node,
    uint32_t reason)
{
    struct kernel_epoll_item *item;
    struct kernel_epoll *epoll;

    (void)reason;
    if (node == 0 || node->context == 0) {
        return;
    }
    item = (struct kernel_epoll_item *)node->context;
    if (item->epoll == 0 || item->oneshot_disarmed) {
        return;
    }
    epoll = item->epoll;
    if (!item->on_ready_list) {
        item->ready_next = 0;
        if (epoll->ready_tail != 0) {
            epoll->ready_tail->ready_next = item;
        } else {
            epoll->ready_head = item;
        }
        epoll->ready_tail = item;
        item->on_ready_list = 1U;
    }
    kernel_wait_queue_wake_all(&epoll->wait_queue);
}

static void epoll_remove_from_ready_list(
    struct kernel_epoll *epoll,
    struct kernel_epoll_item *item)
{
    struct kernel_epoll_item *curr;
    struct kernel_epoll_item *prev;

    if (epoll == 0 || item == 0 || !item->on_ready_list) {
        return;
    }
    curr = epoll->ready_head;
    prev = 0;
    while (curr != 0) {
        if (curr == item) {
            if (prev != 0) {
                prev->ready_next = curr->ready_next;
            } else {
                epoll->ready_head = curr->ready_next;
            }
            if (epoll->ready_tail == curr) {
                epoll->ready_tail = prev;
            }
            curr->ready_next = 0;
            curr->on_ready_list = 0U;
            break;
        }
        prev = curr;
        curr = curr->ready_next;
    }
}

int kernel_epoll_create(
    struct kernel_heap *heap,
    struct kernel_epoll **out_epoll)
{
    struct kernel_epoll *epoll;
    enum kernel_heap_status status;

    if (heap == 0 || out_epoll == 0) {
        return -KERNEL_EINVAL;
    }
    status = kernel_heap_allocate_zeroed(heap,
                                          1U,
                                          sizeof(*epoll),
                                          (void **)&epoll);
    if (status != KERNEL_HEAP_STATUS_OK) {
        return -KERNEL_ENOMEM;
    }
    epoll->heap = heap;
    kernel_wait_queue_init(&epoll->wait_queue);
    epoll->items_head = 0;
    epoll->ready_head = 0;
    epoll->ready_tail = 0;
    epoll->item_count = 0U;
    *out_epoll = epoll;
    return 0;
}

void kernel_epoll_destroy(struct kernel_epoll *epoll)
{
    struct kernel_epoll_item *item;
    struct kernel_epoll_item *next;

    if (epoll == 0) {
        return;
    }
    item = epoll->items_head;
    while (item != 0) {
        next = item->items_next;
        if (item->wait_node.queue != 0) {
            kernel_wait_queue_remove(&item->wait_node);
        }
        if (item->target_file != 0) {
            if (item->target_prev != 0) {
                item->target_prev->target_next = item->target_next;
            } else if (item->target_file->ep_items == item) {
                item->target_file->ep_items = item->target_next;
            }
            if (item->target_next != 0) {
                item->target_next->target_prev = item->target_prev;
            }
        }
        (void)kernel_heap_release(epoll->heap, item);
        item = next;
    }
    epoll->items_head = 0;
    epoll->ready_head = 0;
    epoll->ready_tail = 0;
    epoll->item_count = 0U;
    (void)kernel_heap_release(epoll->heap, epoll);
}

void kernel_epoll_notify_file_release(
    struct kernel_open_file_description *file)
{
    struct kernel_epoll_item *item;
    struct kernel_epoll_item *next;

    if (file == 0) {
        return;
    }
    item = file->ep_items;
    while (item != 0) {
        next = item->target_next;
        if (item->wait_node.queue != 0) {
            kernel_wait_queue_remove(&item->wait_node);
        }
        if (item->epoll != 0) {
            epoll_remove_from_ready_list(item->epoll, item);
            if (item->items_prev != 0) {
                item->items_prev->items_next = item->items_next;
            } else if (item->epoll->items_head == item) {
                item->epoll->items_head = item->items_next;
            }
            if (item->items_next != 0) {
                item->items_next->items_prev = item->items_prev;
            }
            if (item->epoll->item_count > 0U) {
                item->epoll->item_count--;
            }
            (void)kernel_heap_release(item->epoll->heap, item);
        }
        item = next;
    }
    file->ep_items = 0;
}

uint32_t kernel_epoll_poll(
    struct kernel_epoll *epoll,
    uint32_t requested_events,
    struct kernel_wait_queue **out_queue)
{
    struct kernel_epoll_item *item;
    uint32_t revents = 0U;

    if (out_queue != 0) {
        *out_queue = 0;
    }
    if (epoll == 0) {
        return KERNEL_POLLNVAL;
    }
    if (out_queue != 0) {
        *out_queue = &epoll->wait_queue;
    }
    item = epoll->ready_head;
    while (item != 0) {
        if (!item->oneshot_disarmed && item->target_file != 0) {
            uint32_t target_revents =
                kernel_open_file_poll(item->target_file, item->events, 0);
            if ((target_revents & (item->events | KERNEL_POLLERR | KERNEL_POLLHUP)) != 0U) {
                revents |= (KERNEL_POLLIN | KERNEL_POLLRDNORM);
                break;
            }
        }
        item = item->ready_next;
    }
    return revents & requested_events;
}

enum kernel_files_status kernel_files_epoll_create1(
    struct kernel_files *files,
    uint32_t flags,
    int64_t *linux_result)
{
    struct kernel_epoll *epoll = 0;
    struct kernel_open_file_description *ofd = 0;
    uint32_t fd = 0U;
    int err;
    int find_result;

    if (!kernel_files_is_live(files) || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    if ((flags & ~KERNEL_EPOLL_CLOEXEC) != 0U) {
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    err = kernel_epoll_create(files->heap, &epoll);
    if (err != 0) {
        *linux_result = err;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_open_file_create_epoll(files->heap, epoll, flags, &ofd) !=
        KERNEL_OPEN_FILE_STATUS_OK) {
        kernel_epoll_destroy(epoll);
        *linux_result = -KERNEL_ENOMEM;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_files_find_free_fd(files, &fd, &find_result) !=
        KERNEL_FILES_STATUS_OK) {
        (void)kernel_open_file_release(&ofd);
        *linux_result = (int64_t)find_result;
        return KERNEL_FILES_STATUS_OK;
    }
    files->record->slots[fd].description = ofd;
    if ((flags & KERNEL_EPOLL_CLOEXEC) != 0U) {
        files->record->slots[fd].flags |= KERNEL_FILES_FD_CLOEXEC;
    }
    *linux_result = (int64_t)fd;
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_files_epoll_ctl(
    struct kernel_files *files,
    int64_t epfd,
    int32_t op,
    int64_t fd,
    uint32_t events,
    uint64_t data,
    int64_t *linux_result)
{
    struct kernel_open_file_description *epoll_file;
    struct kernel_open_file_description *target_file;
    struct kernel_epoll *epoll;
    struct kernel_epoll_item *item;
    enum kernel_heap_status heap_status;
    struct kernel_wait_queue *target_queue;
    uint32_t current_revents;

    if (!kernel_files_is_live(files) || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    epoll_file = kernel_files_lookup_description(files, epfd);
    if (epoll_file == 0) {
        *linux_result = -KERNEL_EBADF;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_open_file_kind(epoll_file) != KERNEL_OPEN_FILE_KIND_EPOLL ||
        epoll_file->epoll == 0) {
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    if (epfd == fd) {
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    target_file = kernel_files_lookup_description(files, fd);
    if (target_file == 0) {
        *linux_result = -KERNEL_EBADF;
        return KERNEL_FILES_STATUS_OK;
    }
    if (target_file == epoll_file) {
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    epoll = epoll_file->epoll;

    switch (op) {
    case KERNEL_EPOLL_CTL_ADD:
        item = epoll->items_head;
        while (item != 0) {
            if (item->target_fd == (int)fd) {
                *linux_result = -KERNEL_EEXIST;
                return KERNEL_FILES_STATUS_OK;
            }
            item = item->items_next;
        }
        heap_status = kernel_heap_allocate_zeroed(epoll->heap,
                                                  1U,
                                                  sizeof(*item),
                                                  (void **)&item);
        if (heap_status != KERNEL_HEAP_STATUS_OK) {
            *linux_result = -KERNEL_ENOMEM;
            return KERNEL_FILES_STATUS_OK;
        }
        item->target_fd = (int)fd;
        item->target_file = target_file;
        item->epoll = epoll;
        item->events = events;
        item->data = data;
        item->oneshot_disarmed = 0U;
        item->on_ready_list = 0U;

        /* Link into epoll items list */
        item->items_prev = 0;
        item->items_next = epoll->items_head;
        if (epoll->items_head != 0) {
            epoll->items_head->items_prev = item;
        }
        epoll->items_head = item;
        epoll->item_count++;

        /* Link into target_file ep_items list */
        item->target_prev = 0;
        item->target_next = target_file->ep_items;
        if (target_file->ep_items != 0) {
            target_file->ep_items->target_prev = item;
        }
        target_file->ep_items = item;

        /* Register wait node on target wait queue */
        kernel_wait_node_init_callback(&item->wait_node,
                                       kernel_epoll_wait_callback,
                                       item);
        target_queue = 0;
        current_revents = kernel_open_file_poll(target_file, events, &target_queue);
        if (target_queue != 0) {
            kernel_wait_queue_add(target_queue, &item->wait_node);
        }

        /* If already ready, place on ready list */
        if ((current_revents & (events | KERNEL_POLLERR | KERNEL_POLLHUP)) != 0U) {
            item->ready_next = 0;
            if (epoll->ready_tail != 0) {
                epoll->ready_tail->ready_next = item;
            } else {
                epoll->ready_head = item;
            }
            epoll->ready_tail = item;
            item->on_ready_list = 1U;
            kernel_wait_queue_wake_all(&epoll->wait_queue);
        }
        *linux_result = 0;
        return KERNEL_FILES_STATUS_OK;

    case KERNEL_EPOLL_CTL_MOD:
        item = epoll->items_head;
        while (item != 0) {
            if (item->target_fd == (int)fd) {
                break;
            }
            item = item->items_next;
        }
        if (item == 0) {
            *linux_result = -KERNEL_ENOENT;
            return KERNEL_FILES_STATUS_OK;
        }
        item->events = events;
        item->data = data;
        item->oneshot_disarmed = 0U;

        current_revents = kernel_open_file_poll(target_file, events, 0);
        if ((current_revents & (events | KERNEL_POLLERR | KERNEL_POLLHUP)) != 0U) {
            if (!item->on_ready_list) {
                item->ready_next = 0;
                if (epoll->ready_tail != 0) {
                    epoll->ready_tail->ready_next = item;
                } else {
                    epoll->ready_head = item;
                }
                epoll->ready_tail = item;
                item->on_ready_list = 1U;
                kernel_wait_queue_wake_all(&epoll->wait_queue);
            }
        }
        *linux_result = 0;
        return KERNEL_FILES_STATUS_OK;

    case KERNEL_EPOLL_CTL_DEL:
        item = epoll->items_head;
        while (item != 0) {
            if (item->target_fd == (int)fd) {
                break;
            }
            item = item->items_next;
        }
        if (item == 0) {
            *linux_result = -KERNEL_ENOENT;
            return KERNEL_FILES_STATUS_OK;
        }
        if (item->wait_node.queue != 0) {
            kernel_wait_queue_remove(&item->wait_node);
        }
        epoll_remove_from_ready_list(epoll, item);

        if (item->target_prev != 0) {
            item->target_prev->target_next = item->target_next;
        } else if (target_file->ep_items == item) {
            target_file->ep_items = item->target_next;
        }
        if (item->target_next != 0) {
            item->target_next->target_prev = item->target_prev;
        }

        if (item->items_prev != 0) {
            item->items_prev->items_next = item->items_next;
        } else if (epoll->items_head == item) {
            epoll->items_head = item->items_next;
        }
        if (item->items_next != 0) {
            item->items_next->items_prev = item->items_prev;
        }
        if (epoll->item_count > 0U) {
            epoll->item_count--;
        }
        (void)kernel_heap_release(epoll->heap, item);
        *linux_result = 0;
        return KERNEL_FILES_STATUS_OK;

    default:
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
}

static int epoll_calculate_deadline(
    int32_t timeout_ms,
    int *out_has_timeout,
    int *out_immediate,
    uint64_t *out_deadline)
{
    if (timeout_ms < 0) {
        *out_has_timeout = 0;
        *out_immediate = 0;
        *out_deadline = 0U;
        return 0;
    }
    if (timeout_ms == 0) {
        *out_has_timeout = 1;
        *out_immediate = 1;
        *out_deadline = 0U;
        return 0;
    }
    *out_has_timeout = 1;
    {
        uint64_t duration_ns = (uint64_t)timeout_ms * 1000000ULL;
        uint64_t now_ns = kernel_time_monotonic_ns();
        uint64_t target_monotonic_ns =
            duration_ns > (uint64_t)INT64_MAX - now_ns ? (uint64_t)INT64_MAX
                                                       : now_ns + duration_ns;
        enum kernel_time_status time_status =
            kernel_time_deadline_from_monotonic(target_monotonic_ns, out_deadline);
        if (time_status == KERNEL_TIME_STATUS_DEADLINE_PASSED) {
            *out_immediate = 1;
            *out_deadline = 0U;
            return 0;
        }
        if (time_status != KERNEL_TIME_STATUS_OK) {
            return -KERNEL_EINVAL;
        }
    }
    *out_immediate = 0;
    return 0;
}

static int epoll_setup_sigmask(
    struct kernel_mm *mm,
    struct kernel_task *task,
    uint64_t user_sigmask,
    size_t sigsetsize,
    uint64_t *saved_mask,
    int *mask_modified)
{
    uint64_t new_mask = 0U;
    size_t copied = 0U;
    enum kernel_uaccess_status status;

    *mask_modified = 0;
    if (user_sigmask == 0U) {
        return 0;
    }
    if (sigsetsize != sizeof(uint64_t)) {
        return -KERNEL_EINVAL;
    }
    status = kernel_copy_from_user(mm, &new_mask, user_sigmask, sizeof(new_mask), &copied);
    if (status == KERNEL_UACCESS_STATUS_FAULT) {
        return -KERNEL_EFAULT;
    }
    if (status != KERNEL_UACCESS_STATUS_OK || copied != sizeof(new_mask)) {
        return -KERNEL_EINVAL;
    }
    if (kernel_signal_set_temporary_mask(task, new_mask, saved_mask) !=
        KERNEL_SIGNAL_STATUS_OK) {
        return -KERNEL_EINVAL;
    }
    *mask_modified = 1;
    return 0;
}

enum kernel_files_status kernel_files_epoll_pwait(
    struct kernel_files *files,
    struct kernel_mm *mm,
    struct kernel_task *task,
    int64_t epfd,
    uint64_t user_events,
    int32_t maxevents,
    int32_t timeout,
    uint64_t user_sigmask,
    size_t sigsetsize,
    int64_t *linux_result)
{
    struct kernel_open_file_description *epoll_file = 0;
    struct kernel_epoll *epoll;
    struct linux_epoll_event stack_events[KERNEL_EPOLL_STACK_CAPACITY];
    struct linux_epoll_event *event_buffer = stack_events;
    size_t buffer_capacity;
    int has_timeout = 0;
    int immediate = 0;
    uint64_t deadline = 0U;
    uint64_t saved_mask = 0U;
    int mask_modified = 0;
    int timeout_error;
    int sig_error;
    enum kernel_files_status pin_status;

    if (!kernel_files_is_live(files) || mm == 0 || task == 0 || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    if (maxevents <= 0 || maxevents > (int32_t)KERNEL_EPOLL_MAX_EVENTS) {
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    if (user_events == 0U) {
        *linux_result = -KERNEL_EFAULT;
        return KERNEL_FILES_STATUS_OK;
    }

    pin_status = kernel_files_pin(files, epfd, &epoll_file, linux_result);
    if (pin_status != KERNEL_FILES_STATUS_OK || epoll_file == 0) {
        return pin_status;
    }
    if (kernel_open_file_kind(epoll_file) != KERNEL_OPEN_FILE_KIND_EPOLL ||
        epoll_file->epoll == 0) {
        (void)kernel_open_file_release(&epoll_file);
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    epoll = epoll_file->epoll;

    timeout_error = epoll_calculate_deadline(timeout, &has_timeout, &immediate, &deadline);
    if (timeout_error != 0) {
        (void)kernel_open_file_release(&epoll_file);
        *linux_result = timeout_error;
        return KERNEL_FILES_STATUS_OK;
    }

    buffer_capacity = (size_t)maxevents;
    if (buffer_capacity > KERNEL_EPOLL_STACK_CAPACITY) {
        enum kernel_heap_status heap_status = kernel_heap_allocate_zeroed(
            files->heap,
            buffer_capacity,
            sizeof(struct linux_epoll_event),
            (void **)&event_buffer);
        if (heap_status != KERNEL_HEAP_STATUS_OK) {
            (void)kernel_open_file_release(&epoll_file);
            *linux_result = -KERNEL_ENOMEM;
            return KERNEL_FILES_STATUS_OK;
        }
    }

    sig_error = epoll_setup_sigmask(mm, task, user_sigmask, sigsetsize, &saved_mask, &mask_modified);
    if (sig_error != 0) {
        if (event_buffer != stack_events) {
            (void)kernel_heap_release(files->heap, event_buffer);
        }
        (void)kernel_open_file_release(&epoll_file);
        *linux_result = sig_error;
        return KERNEL_FILES_STATUS_OK;
    }

    for (;;) {
        size_t ready_count = 0U;
        struct kernel_epoll_item *item = epoll->ready_head;
        struct kernel_epoll_item *requeue_head = 0;
        struct kernel_epoll_item *requeue_tail = 0;

        /* Harvest from ready list */
        while (item != 0 && ready_count < buffer_capacity) {
            struct kernel_epoll_item *next_item = item->ready_next;
            epoll->ready_head = next_item;
            if (epoll->ready_head == 0) {
                epoll->ready_tail = 0;
            }
            item->ready_next = 0;
            item->on_ready_list = 0U;

            if (!item->oneshot_disarmed && item->target_file != 0) {
                uint32_t revents = kernel_open_file_poll(item->target_file, item->events, 0);
                revents &= (item->events | KERNEL_POLLERR | KERNEL_POLLHUP);
                if (revents != 0U) {
                    event_buffer[ready_count].events = revents;
                    event_buffer[ready_count]._pad = 0U;
                    event_buffer[ready_count].data = item->data;
                    ready_count++;

                    if ((item->events & KERNEL_EPOLLONESHOT) != 0U) {
                        item->oneshot_disarmed = 1U;
                    } else if ((item->events & KERNEL_EPOLLET) == 0U) {
                        /* Level-triggered: stage to re-add to ready list */
                        item->ready_next = 0;
                        if (requeue_tail != 0) {
                            requeue_tail->ready_next = item;
                        } else {
                            requeue_head = item;
                        }
                        requeue_tail = item;
                        item->on_ready_list = 1U;
                    }
                }
            }
            item = next_item;
        }

        /* Requeue level-triggered items that are still ready */
        if (requeue_head != 0) {
            if (epoll->ready_tail != 0) {
                epoll->ready_tail->ready_next = requeue_head;
            } else {
                epoll->ready_head = requeue_head;
            }
            epoll->ready_tail = requeue_tail;
        }

        if (ready_count > 0U) {
            size_t copied = 0U;
            enum kernel_uaccess_status u_status = kernel_copy_to_user(
                mm,
                user_events,
                event_buffer,
                ready_count * sizeof(struct linux_epoll_event),
                &copied);
            if (u_status != KERNEL_UACCESS_STATUS_OK ||
                copied != ready_count * sizeof(struct linux_epoll_event)) {
                *linux_result = -KERNEL_EFAULT;
            } else {
                *linux_result = (int64_t)ready_count;
            }
            break;
        }

        if (immediate != 0) {
            *linux_result = 0;
            break;
        }

        /* Block on epoll wait queue */
        {
            enum kernel_wait_wake_reason wake_reason = KERNEL_WAIT_WOKEN;
            uint64_t saved_intr = riscv_interrupt_save();
            enum kernel_scheduler_status sched_status =
                kernel_scheduler_block_current(&epoll->wait_queue,
                                               deadline,
                                               1,
                                               &wake_reason);
            riscv_interrupt_restore(saved_intr);
            if (sched_status != KERNEL_SCHEDULER_STATUS_OK) {
                *linux_result = -KERNEL_EIO;
                break;
            }
            if (wake_reason == KERNEL_WAIT_SIGNALLED) {
                *linux_result = -KERNEL_EINTR;
                break;
            }
            if (wake_reason == KERNEL_WAIT_TIMEOUT) {
                *linux_result = 0;
                break;
            }
        }
    }

    if (mask_modified) {
        int interrupted = (*linux_result == -KERNEL_EINTR);
        kernel_signal_restore_temporary_mask(task, saved_mask, interrupted);
    }
    if (event_buffer != stack_events) {
        (void)kernel_heap_release(files->heap, event_buffer);
    }
    (void)kernel_open_file_release(&epoll_file);
    return KERNEL_FILES_STATUS_OK;
}
