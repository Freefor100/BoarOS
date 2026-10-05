#include "epoll_internal.h"
#include "private.h"
#include "../open_file_internal.h"

#include <arch/riscv/context.h>
#include <kernel/errno.h>
#include <kernel/epoll.h>
#include <kernel/task.h>
#include <kernel/heap.h>
#include <kernel/open_file.h>
#include <kernel/scheduler.h>
#include <kernel/signal.h>
#include <kernel/socket.h>
#include <kernel/time.h>
#include <kernel/uaccess.h>
#include <kernel/files.h>

#include <stddef.h>
#include <stdint.h>

#define KERNEL_EPOLL_MAX_NESTS 4U

static void epoll_ready_add(struct kernel_epoll *epoll, struct kernel_epoll_item *item)
{
    if (!item->linked || item->oneshot_disarmed || item->on_ready_list) return;
    item->ready_next = 0;
    if (epoll->ready_tail) epoll->ready_tail->ready_next = item;
    else epoll->ready_head = item;
    epoll->ready_tail = item;
    item->on_ready_list = 1;
}

static void epoll_pending_add(struct kernel_epoll *epoll, struct kernel_epoll_item *item)
{
    if (!item->linked || item->oneshot_disarmed || item->on_pending_list) return;
    item->pending_next = 0;
    if (epoll->pending_tail) epoll->pending_tail->pending_next = item;
    else epoll->pending_head = item;
    epoll->pending_tail = item;
    item->on_pending_list = 1;
}

static void epoll_item_put(struct kernel_epoll_item *item)
{
    if (!item->references) __builtin_trap();
    if (!--item->references && kernel_heap_release(item->heap, item) != KERNEL_HEAP_STATUS_OK)
        __builtin_trap();
}

static void kernel_epoll_wait_callback(struct kernel_wait_node *node, uint32_t reason)
{
    (void)reason;
    if (!node || !node->context) return;
    uintptr_t irq = riscv_interrupt_save();
    struct kernel_epoll_item *item = node->context;
    struct kernel_epoll *epoll = item->epoll;
    if (epoll && item->linked && !item->oneshot_disarmed) {
        item->notification_generation++;
        /* poll和用户复制都可能重入；通知链不借用扫描者的ready_next。 */
        if (epoll->scan_owner) epoll_pending_add(epoll, item);
        else epoll_ready_add(epoll, item);
        kernel_wait_queue_wake_all(&epoll->wait_queue);
    }
    riscv_interrupt_restore(irq);
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

static enum kernel_files_status epoll_item_unlink_and_destroy(
    struct kernel_epoll_item *item)
{
    if (item == 0 || !item->linked) {
        return KERNEL_FILES_STATUS_OK;
    }
    uintptr_t irq = riscv_interrupt_save();
    struct kernel_epoll *epoll = item->epoll;
    struct kernel_open_file_description *target_file = item->target_file;

    if (item->wait_node.queue != 0) {
        kernel_wait_queue_remove(&item->wait_node);
    }

    if (epoll != 0) {
        epoll_remove_from_ready_list(epoll, item);
        if (item->on_pending_list) {
            struct kernel_epoll_item **link = &epoll->pending_head, *previous = 0;
            while (*link && *link != item) { previous = *link; link = &(*link)->pending_next; }
            if (!*link) __builtin_trap();
            *link = item->pending_next;
            if (epoll->pending_tail == item) epoll->pending_tail = previous;
            item->pending_next = 0;
            item->on_pending_list = 0;
        }
        if (item->items_prev != 0) {
            item->items_prev->items_next = item->items_next;
        } else if (epoll->items_head == item) {
            epoll->items_head = item->items_next;
        }
        if (item->items_next != 0) {
            item->items_next->items_prev = item->items_prev;
        }
        item->items_prev = 0;
        item->items_next = 0;
        if (epoll->item_count > 0U) {
            epoll->item_count--;
        }
    }

    if (target_file != 0) {
        if (item->target_prev != 0) {
            item->target_prev->target_next = item->target_next;
        } else if (target_file->ep_items == item) {
            target_file->ep_items = item->target_next;
        }
        if (item->target_next != 0) {
            item->target_next->target_prev = item->target_prev;
        }
        item->target_prev = 0;
        item->target_next = 0;
        item->target_file = 0;
    }

    item->linked = 0;
    item->epoll = 0;
    epoll_item_put(item);
    riscv_interrupt_restore(irq);
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_epoll_destroy(struct kernel_epoll *epoll)
{
    if (epoll == 0) {
        return KERNEL_FILES_STATUS_OK;
    }
    if (epoll->scan_owner) __builtin_trap();
    while (epoll->items_head != 0) {
        struct kernel_epoll_item *item = epoll->items_head;
        (void)epoll_item_unlink_and_destroy(item);
    }
    epoll->items_head = 0;
    epoll->ready_head = 0;
    epoll->ready_tail = 0;
    epoll->item_count = 0U;

    (void)kernel_heap_release(epoll->heap, epoll);
    return KERNEL_FILES_STATUS_OK;
}

void kernel_epoll_notify_file_release(
    struct kernel_open_file_description *file)
{
    if (file == 0) {
        return;
    }
    while (file->ep_items != 0) {
        (void)epoll_item_unlink_and_destroy(file->ep_items);
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
        (void)kernel_epoll_destroy(epoll);
        *linux_result = -KERNEL_ENOMEM;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_files_find_free_fd(files, &fd, &find_result) !=
        KERNEL_FILES_STATUS_OK) {
        (void)kernel_open_file_release(&ofd);
        return KERNEL_FILES_STATUS_STATE;
    }
    if (find_result != 0) {
        (void)kernel_open_file_release(&ofd);
        *linux_result = (int64_t)find_result;
        return KERNEL_FILES_STATUS_OK;
    }
    if (kernel_files_install_new_owned_at(
            files,
            fd,
            (flags & KERNEL_EPOLL_CLOEXEC) != 0U
                ? KERNEL_FILES_FD_CLOEXEC
                : 0U,
            &ofd) != KERNEL_FILES_STATUS_OK) {
        (void)kernel_open_file_release(&ofd);
        return KERNEL_FILES_STATUS_STATE;
    }
    *linux_result = (int64_t)fd;
    return KERNEL_FILES_STATUS_OK;
}

static struct kernel_epoll_item *epoll_find_item(
    struct kernel_epoll *epoll,
    int fd,
    struct kernel_open_file_description *target_file)
{
    struct kernel_epoll_item *item = epoll->items_head;
    while (item != 0) {
        if (item->target_fd == fd && item->target_file == target_file) {
            return item;
        }
        item = item->items_next;
    }
    return 0;
}

struct epoll_down_frame {
    struct kernel_epoll_item *item;
    int depth;
};

static int epoll_check_downward(
    struct kernel_epoll *inserting_into,
    struct kernel_epoll *target_epoll,
    int *out_down_depth)
{
    struct epoll_down_frame stack[8];
    int top = 0;
    int max_depth = 0;

    stack[0].item = target_epoll->items_head;
    stack[0].depth = 0;

    while (top >= 0) {
        struct kernel_epoll_item *item = stack[top].item;
        if (item == 0) {
            top--;
            continue;
        }
        stack[top].item = item->items_next;

        if (item->target_file != 0 &&
            kernel_open_file_kind(item->target_file) == KERNEL_OPEN_FILE_KIND_EPOLL &&
            item->target_file->epoll != 0) {
            struct kernel_epoll *child_ep = item->target_file->epoll;
            if (child_ep == inserting_into) {
                return -KERNEL_ELOOP;
            }
            int d = stack[top].depth + 1;
            if (d > max_depth) {
                max_depth = d;
            }
            if (d > (int)KERNEL_EPOLL_MAX_NESTS) {
                return -KERNEL_ELOOP;
            }
            if (top + 1 < 8) {
                top++;
                stack[top].item = child_ep->items_head;
                stack[top].depth = d;
            }
        }
    }
    *out_down_depth = max_depth;
    return 0;
}

struct epoll_up_frame {
    struct kernel_epoll_item *item;
    int depth;
};

static int epoll_check_upward(
    struct kernel_epoll *inserting_into,
    int *out_up_depth)
{
    struct epoll_up_frame stack[8];
    int top = 0;
    int max_depth = 0;

    if (inserting_into->file == 0) {
        *out_up_depth = 0;
        return 0;
    }

    stack[0].item = inserting_into->file->ep_items;
    stack[0].depth = 0;

    while (top >= 0) {
        struct kernel_epoll_item *item = stack[top].item;
        if (item == 0) {
            top--;
            continue;
        }
        stack[top].item = item->target_next;

        if (item->epoll != 0) {
            struct kernel_epoll *parent_ep = item->epoll;
            int d = stack[top].depth + 1;
            if (d > max_depth) {
                max_depth = d;
            }
            if (d > (int)KERNEL_EPOLL_MAX_NESTS) {
                return -KERNEL_ELOOP;
            }
            if (parent_ep->file != 0 && top + 1 < 8) {
                top++;
                stack[top].item = parent_ep->file->ep_items;
                stack[top].depth = d;
            }
        }
    }
    *out_up_depth = max_depth;
    return 0;
}

static int epoll_check_nesting(
    struct kernel_epoll *inserting_into,
    struct kernel_epoll *target_epoll)
{
    if (inserting_into == target_epoll) {
        return -KERNEL_EINVAL;
    }
    int down_depth = 0;
    int err = epoll_check_downward(inserting_into, target_epoll, &down_depth);
    if (err != 0) {
        return err;
    }
    int up_depth = 0;
    err = epoll_check_upward(inserting_into, &up_depth);
    if (err != 0) {
        return err;
    }
    if (down_depth + 1 + up_depth > (int)KERNEL_EPOLL_MAX_NESTS) {
        return -KERNEL_ELOOP;
    }
    return 0;
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
    struct kernel_open_file_description *epoll_file = 0;
    struct kernel_open_file_description *target_file = 0;
    struct kernel_epoll *epoll;
    struct kernel_epoll_item *item;
    enum kernel_heap_status heap_status;
    struct kernel_wait_queue *target_queue;
    uint32_t current_revents;
    enum kernel_files_status pin_status;

    if (!kernel_files_is_live(files) || linux_result == 0) {
        return KERNEL_FILES_STATUS_INVALID_ARGUMENT;
    }
    pin_status = kernel_files_pin_data(files, epfd, &epoll_file, linux_result);
    if (pin_status != KERNEL_FILES_STATUS_OK || *linux_result != 0 || epoll_file == 0) {
        return pin_status;
    }
    if (kernel_open_file_kind(epoll_file) != KERNEL_OPEN_FILE_KIND_EPOLL ||
        epoll_file->epoll == 0) {
        (void)kernel_open_file_release(&epoll_file);
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    if (epfd == fd) {
        (void)kernel_open_file_release(&epoll_file);
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    pin_status = kernel_files_pin_data(files, fd, &target_file, linux_result);
    if (pin_status != KERNEL_FILES_STATUS_OK || *linux_result != 0 || target_file == 0) {
        (void)kernel_open_file_release(&epoll_file);
        return pin_status;
    }
    if (target_file == epoll_file) {
        (void)kernel_open_file_release(&target_file);
        (void)kernel_open_file_release(&epoll_file);
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    epoll = epoll_file->epoll;

    switch (op) {
    case KERNEL_EPOLL_CTL_ADD:
        if (!kernel_open_file_supports_epoll(target_file)) {
            *linux_result = -KERNEL_EPERM;
            break;
        }
        if (kernel_open_file_kind(target_file) == KERNEL_OPEN_FILE_KIND_EPOLL) {
            int loop_err = epoll_check_nesting(epoll, target_file->epoll);
            if (loop_err != 0) {
                *linux_result = loop_err;
                break;
            }
        }
        if (epoll_find_item(epoll, (int)fd, target_file) != 0) {
            *linux_result = -KERNEL_EEXIST;
            break;
        }
        heap_status = kernel_heap_allocate_zeroed(epoll->heap,
                                                  1U,
                                                  sizeof(*item),
                                                  (void **)&item);
        if (heap_status != KERNEL_HEAP_STATUS_OK) {
            *linux_result = -KERNEL_ENOMEM;
            break;
        }
        item->heap = epoll->heap;
        item->references = 1;
        item->linked = 1;
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

        if ((current_revents & (events | KERNEL_POLLERR | KERNEL_POLLHUP)) != 0U)
            kernel_epoll_wait_callback(&item->wait_node, 0);
        *linux_result = 0;
        break;

    case KERNEL_EPOLL_CTL_MOD:
        item = epoll_find_item(epoll, (int)fd, target_file);
        if (item == 0) {
            *linux_result = -KERNEL_ENOENT;
            break;
        }
        item->control_generation++;
        item->events = events;
        item->data = data;
        item->oneshot_disarmed = 0U;

        current_revents = kernel_open_file_poll(target_file, events, 0);
        if ((current_revents & (events | KERNEL_POLLERR | KERNEL_POLLHUP)) != 0U)
            kernel_epoll_wait_callback(&item->wait_node, 0);
        *linux_result = 0;
        break;

    case KERNEL_EPOLL_CTL_DEL:
        item = epoll_find_item(epoll, (int)fd, target_file);
        if (item == 0) {
            *linux_result = -KERNEL_ENOENT;
            break;
        }
        epoll_item_unlink_and_destroy(item);
        *linux_result = 0;
        break;

    default:
        *linux_result = -KERNEL_EINVAL;
        break;
    }

    (void)kernel_open_file_release(&target_file);
    (void)kernel_open_file_release(&epoll_file);
    return KERNEL_FILES_STATUS_OK;
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

static void epoll_scan_finish(struct kernel_epoll_wait_request *request)
{
    struct kernel_epoll *epoll = request->epoll;
    uintptr_t irq = riscv_interrupt_save();
    if (request->target && kernel_open_file_release(&request->target) != KERNEL_OPEN_FILE_STATUS_OK)
        __builtin_trap();
    if (epoll->scan_owner == request) {
        /* 尚未交付的项目仍由扫描引用保活；DEL只撤销注册引用。 */
        while (request->head) {
            struct kernel_epoll_item *item = request->head;
            request->head = item->ready_next;
            item->ready_next = 0;
            item->in_scan = 0;
            epoll_ready_add(epoll, item);
            epoll_item_put(item);
        }
        while (epoll->pending_head) {
            struct kernel_epoll_item *item = epoll->pending_head;
            epoll->pending_head = item->pending_next;
            item->pending_next = 0;
            item->on_pending_list = 0;
            epoll_ready_add(epoll, item);
        }
        epoll->pending_tail = 0;
        epoll->scan_owner = 0;
        kernel_wait_queue_wake_all(&epoll->wait_queue);
    }
    riscv_interrupt_restore(irq);
}

void kernel_epoll_abort_wait(struct kernel_epoll_wait_request *request)
{
    if (!request || !request->task) return;
    epoll_scan_finish(request);
    if (kernel_task_epoll_clear(request->task, request) != KERNEL_TASK_STATUS_OK)
        __builtin_trap();
    request->task = 0;
    if (kernel_open_file_release(&request->file) != KERNEL_OPEN_FILE_STATUS_OK)
        __builtin_trap();
}

static int64_t epoll_deliver(struct kernel_epoll_wait_request *request,
    struct kernel_mm *mm, uint64_t user_events, size_t maximum)
{
    struct kernel_epoll *epoll = request->epoll;
    uintptr_t irq = riscv_interrupt_save();
    if (epoll->scan_owner || !epoll->ready_head) {
        riscv_interrupt_restore(irq);
        return 0;
    }
    epoll->scan_owner = request;
    request->head = epoll->ready_head;
    epoll->ready_head = epoll->ready_tail = 0;
    for (struct kernel_epoll_item *item = request->head; item; item = item->ready_next) {
        if (item->references == UINT32_MAX || item->in_scan) __builtin_trap();
        item->references++;
        item->in_scan = 1;
        item->on_ready_list = 0;
    }
    riscv_interrupt_restore(irq);
    int64_t delivered = 0;
    while (request->head && (size_t)delivered < maximum) {
        struct kernel_epoll_item *item = request->head;
        uint64_t control = item->control_generation;
        uint64_t notification = item->notification_generation;
        uint32_t interest = item->events;
        struct linux_epoll_event event = {.data = item->data};
        if (item->linked && !item->oneshot_disarmed && item->target_file) {
            request->target = item->target_file;
            if (kernel_open_file_acquire(request->target) != KERNEL_OPEN_FILE_STATUS_OK)
                __builtin_trap();
            event.events = kernel_open_file_poll(request->target, interest, 0) &
                (interest | KERNEL_POLLERR | KERNEL_POLLHUP);
            if (item->linked && event.events && control == item->control_generation) {
                size_t copied = 0;
                enum kernel_uaccess_status status = kernel_copy_to_user(mm,
                    user_events + (uint64_t)delivered * sizeof(event), &event, sizeof(event), &copied);
                if (status != KERNEL_UACCESS_STATUS_OK || copied != sizeof(event)) {
                    if (status != KERNEL_UACCESS_STATUS_OK && status != KERNEL_UACCESS_STATUS_FAULT)
                        __builtin_trap();
                    if (!delivered) delivered = -KERNEL_EFAULT;
                    break;
                }
                delivered++;
                /* 完整event复制是提交点；睡眠期间的MOD不能被旧ONESHOT覆盖。 */
                if (item->linked && control == item->control_generation) {
                    if (interest & KERNEL_EPOLLONESHOT) item->oneshot_disarmed = 1;
                    else if (!(interest & KERNEL_EPOLLET)) epoll_pending_add(epoll, item);
                }
            }
            if (kernel_open_file_release(&request->target) != KERNEL_OPEN_FILE_STATUS_OK)
                __builtin_trap();
        }
        irq = riscv_interrupt_save();
        if (item->linked && (control != item->control_generation ||
                            notification != item->notification_generation))
            epoll_pending_add(epoll, item);
        request->head = item->ready_next;
        item->ready_next = 0;
        item->in_scan = 0;
        epoll_item_put(item);
        riscv_interrupt_restore(irq);
    }
    epoll_scan_finish(request);
    return delivered;
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
    struct kernel_epoll_wait_request request = {0};
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
    if (maxevents <= 0) {
        *linux_result = -KERNEL_EINVAL;
        return KERNEL_FILES_STATUS_OK;
    }
    if (user_events == 0U) {
        *linux_result = -KERNEL_EFAULT;
        return KERNEL_FILES_STATUS_OK;
    }

    pin_status = kernel_files_pin_data(files, epfd, &epoll_file, linux_result);
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

    request = (struct kernel_epoll_wait_request){.task = task, .epoll = epoll, .file = epoll_file};
    if (kernel_task_epoll_register(task, &request) != KERNEL_TASK_STATUS_OK) __builtin_trap();

    sig_error = epoll_setup_sigmask(mm, task, user_sigmask, sigsetsize, &saved_mask, &mask_modified);
    if (sig_error != 0) {
        kernel_epoll_abort_wait(&request);
        *linux_result = sig_error;
        return KERNEL_FILES_STATUS_OK;
    }

    for (;;) {
        int64_t delivered = epoll_deliver(&request, mm, user_events, (size_t)maxevents);
        if (delivered) {
            *linux_result = delivered;
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
            uint64_t sleep_deadline = deadline;
            if (!epoll->scan_owner && epoll->ready_head != 0) {
                riscv_interrupt_restore(saved_intr);
                continue;
            }
            enum kernel_scheduler_status sched_status =
                kernel_scheduler_block_current(&epoll->wait_queue,
                                               sleep_deadline,
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
    kernel_epoll_abort_wait(&request);
    return KERNEL_FILES_STATUS_OK;
}
