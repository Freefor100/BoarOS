#include "epoll_internal.h"
#include "epoll_state.h"
#include "private.h"
#include "../open_file_internal.h"

#include <arch/context.h>
#include <kernel/errno.h>
#include <kernel/cpu.h>
#include <kernel/epoll.h>
#include <kernel/task.h>
#include <kernel/heap.h>
#include <kernel/open_file.h>
#include <kernel/scheduler.h>
#include <kernel/signal.h>
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
    item->ready_prev = epoll->ready_tail;
    if (epoll->ready_tail) epoll->ready_tail->ready_next = item;
    else epoll->ready_head = item;
    epoll->ready_tail = item;
    item->on_ready_list = 1;
}

static void epoll_pending_add(struct kernel_epoll *epoll, struct kernel_epoll_item *item)
{
    if (!item->linked || item->oneshot_disarmed || item->on_pending_list) return;
    item->pending_next = 0;
    item->pending_prev = epoll->pending_tail;
    if (epoll->pending_tail) epoll->pending_tail->pending_next = item;
    else epoll->pending_head = item;
    epoll->pending_tail = item;
    item->on_pending_list = 1;
}

static void epoll_item_put(struct kernel_epoll_item *item)
{
    EPOLL_STATE(irq);
    if (!item->references) __builtin_trap();
    int release = !--item->references;
    epoll_state_release(&irq);
    if (release) kernel_heap_release(item->heap, item);
}

static void epoll_notify(struct kernel_epoll *epoll)
{
    uint64_t sequence = kernel_wait_notification_sequence();
    if (sequence && epoll->notification_sequence == sequence) return;
    epoll->notification_sequence = sequence;
    epoll_state_notify(&epoll->wait_queue);
}

static void kernel_epoll_wait_callback(struct kernel_wait_node *node, uint32_t reason)
{
    if (!node || !node->context) return;
    EPOLL_STATE(irq);
    struct kernel_epoll_item *item = node->context;
    struct kernel_epoll *epoll = item->epoll;
    if (reason==KERNEL_WAIT_POLL_FREE) {
        /* The callback owns a cursor borrow. Detach without waiting for
         * itself; DEL/close still drains this borrow before freeing item. */
        kernel_wait_queue_remove(node);
        return;
    }
    if (epoll && item->linked && !item->oneshot_disarmed) {
        item->notification_generation++;
        /* poll和用户复制都可能重入；通知链不借用扫描者的ready_next。 */
        if (epoll->scan_owner && !item->on_ready_list) epoll_pending_add(epoll, item);
        else epoll_ready_add(epoll, item);
        epoll_notify(epoll);
    }
    epoll_state_release(&irq);
}

static void epoll_remove_from_ready_list(struct kernel_epoll *epoll, struct kernel_epoll_item *item)
{
    if (!item->on_ready_list) return;
    if (item->ready_prev) item->ready_prev->ready_next = item->ready_next;
    else epoll->ready_head = item->ready_next;
    if (item->ready_next) item->ready_next->ready_prev = item->ready_prev;
    else epoll->ready_tail = item->ready_prev;
    item->ready_next = item->ready_prev = 0;
    item->on_ready_list = 0;
}
static void epoll_remove_from_pending_list(struct kernel_epoll *epoll, struct kernel_epoll_item *item)
{
    if (!item->on_pending_list) return;
    if (item->pending_prev) item->pending_prev->pending_next = item->pending_next;
    else epoll->pending_head = item->pending_next;
    if (item->pending_next) item->pending_next->pending_prev = item->pending_prev;
    else epoll->pending_tail = item->pending_prev;
    item->pending_next = item->pending_prev = 0;
    item->on_pending_list = 0;
}

static unsigned epoll_index_height(struct kernel_epoll_item *item)
{ return item ? item->index_height : 0; }
static void epoll_index_update(struct kernel_epoll_item *item)
{
    unsigned left = epoll_index_height(item->index_left), right = epoll_index_height(item->index_right);
    item->index_height = 1 + (left > right ? left : right);
}
static struct kernel_epoll_item *epoll_index_rotate(struct kernel_epoll *epoll,
    struct kernel_epoll_item *item, int left)
{
    struct kernel_epoll_item *pivot = left ? item->index_right : item->index_left;
    struct kernel_epoll_item *parent = item->index_parent;
    if (!parent) epoll->items_root = pivot;
    else if (parent->index_left == item) parent->index_left = pivot;
    else parent->index_right = pivot;
    pivot->index_parent = parent;
    if (left) {
        item->index_right = pivot->index_left;
        if (item->index_right) item->index_right->index_parent = item;
        pivot->index_left = item;
    } else {
        item->index_left = pivot->index_right;
        if (item->index_left) item->index_left->index_parent = item;
        pivot->index_right = item;
    }
    item->index_parent = pivot;
    epoll_index_update(item); epoll_index_update(pivot);
    return pivot;
}
static void epoll_index_balance(struct kernel_epoll *epoll, struct kernel_epoll_item *item)
{
    while (item) {
        epoll_index_update(item);
        int balance = (int)epoll_index_height(item->index_left) - (int)epoll_index_height(item->index_right);
        if (balance > 1) {
            if (epoll_index_height(item->index_left->index_right) > epoll_index_height(item->index_left->index_left))
                epoll_index_rotate(epoll, item->index_left, 1);
            item = epoll_index_rotate(epoll, item, 0);
        } else if (balance < -1) {
            if (epoll_index_height(item->index_right->index_left) > epoll_index_height(item->index_right->index_right))
                epoll_index_rotate(epoll, item->index_right, 0);
            item = epoll_index_rotate(epoll, item, 1);
        }
        item = item->index_parent;
    }
}
static int epoll_index_compare(int fd, struct kernel_open_file_description *file,
    const struct kernel_epoll_item *item)
{
    if (fd != item->target_fd) return fd < item->target_fd ? -1 : 1;
    return (uintptr_t)file < (uintptr_t)item->target_file ? -1 : (file != item->target_file);
}
static void epoll_index_insert(struct kernel_epoll *epoll, struct kernel_epoll_item *item)
{
    struct kernel_epoll_item **link = &epoll->items_root, *parent = 0;
    while (*link) {
        parent = *link;
        int order = epoll_index_compare(item->target_fd, item->target_file, parent);
        if (!order) __builtin_trap();
        link = order < 0 ? &parent->index_left : &parent->index_right;
    }
    *link = item; item->index_parent = parent; item->index_height = 1;
    epoll_index_balance(epoll, parent);
}
static void epoll_index_remove(struct kernel_epoll *epoll, struct kernel_epoll_item *item)
{
    /* 旋转对象本身而非复制key，扫描者和反向注册继续指向同一item。 */
    while (item->index_left || item->index_right)
        epoll_index_rotate(epoll, item, epoll_index_height(item->index_right) > epoll_index_height(item->index_left));
    struct kernel_epoll_item *parent = item->index_parent;
    if (!parent) epoll->items_root = 0;
    else if (parent->index_left == item) parent->index_left = 0;
    else parent->index_right = 0;
    item->index_parent = 0;
    epoll_index_balance(epoll, parent);
}
static struct kernel_epoll_item *epoll_index_first(struct kernel_epoll *epoll)
{
    struct kernel_epoll_item *item = epoll->items_root;
    while (item && item->index_left) item = item->index_left;
    return item;
}
static struct kernel_epoll_item *epoll_index_next(struct kernel_epoll_item *item)
{
    if (item->index_right) {
        item = item->index_right;
        while (item->index_left) item = item->index_left;
        return item;
    }
    while (item->index_parent && item == item->index_parent->index_right) item = item->index_parent;
    return item->index_parent;
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
    epoll->items_root = 0;
    epoll->ready_head = 0;
    epoll->ready_tail = 0;
    *out_epoll = epoll;
    return 0;
}

static enum kernel_files_status epoll_item_unlink_and_destroy(
    struct kernel_epoll_item *item)
{
    if (!item) return KERNEL_FILES_STATUS_OK;
    EPOLL_STATE(irq);
    if (!item->linked) { epoll_state_release(&irq); return KERNEL_FILES_STATUS_OK; }
    struct kernel_epoll *epoll = item->epoll;
    struct kernel_open_file_description *target_file = item->target_file;

    /* 停止新业务通知，再摘登记；在途callback仍借用item/context。 */
    item->linked = 0;
    epoll_state_release(&irq);
    if (kernel_wait_node_remove_sync(&item->wait_node) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
    epoll_state_acquire(&irq);

    if (epoll != 0) {
        epoll_remove_from_ready_list(epoll, item);
        epoll_remove_from_pending_list(epoll, item);
        epoll_index_remove(epoll, item);
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
    epoll_state_release(&irq);
    epoll_item_put(item);
    return KERNEL_FILES_STATUS_OK;
}

enum kernel_files_status kernel_epoll_destroy(struct kernel_epoll *epoll)
{
    if (epoll == 0) {
        return KERNEL_FILES_STATUS_OK;
    }
    if (epoll->scan_owner) __builtin_trap();
    while (epoll->items_root != 0) {
        struct kernel_epoll_item *item = epoll->items_root;
        (void)epoll_item_unlink_and_destroy(item);
    }
    epoll->items_root = 0;
    epoll->ready_head = 0;
    epoll->ready_tail = 0;
    enum kernel_scheduler_status status = kernel_wait_queue_close(&epoll->wait_queue);
    if (status != KERNEL_SCHEDULER_STATUS_OK && status != KERNEL_SCHEDULER_STATUS_NOT_INITIALIZED)
        __builtin_trap();
    if (kernel_wait_queue_destroy(&epoll->wait_queue) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();

    kernel_heap_release(epoll->heap, epoll);
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


enum kernel_files_status kernel_files_epoll_create1(
    struct kernel_files *files,
    uint32_t flags,
    int64_t *linux_result)
{
    KERNEL_SERVICE_SCOPE(service);
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
    struct kernel_files_fd_reservation reservation __attribute__((cleanup(kernel_files_cancel_reservation)))={files,fd};
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
    reservation.files=0;
    *linux_result = (int64_t)fd;
    return KERNEL_FILES_STATUS_OK;
}

static struct kernel_epoll_item *epoll_find_item(
    struct kernel_epoll *epoll,
    int fd,
    struct kernel_open_file_description *target_file)
{
    struct kernel_epoll_item *item = epoll->items_root;
    while (item) {
        int order = epoll_index_compare(fd, target_file, item);
        if (!order) return item;
        item = order < 0 ? item->index_left : item->index_right;
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

    stack[0].item = epoll_index_first(target_epoll);
    stack[0].depth = 0;

    while (top >= 0) {
        struct kernel_epoll_item *item = stack[top].item;
        if (item == 0) {
            top--;
            continue;
        }
        stack[top].item = epoll_index_next(item);

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
                stack[top].item = epoll_index_first(child_ep);
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
    KERNEL_SERVICE_SCOPE(service);
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
    case KERNEL_EPOLL_CTL_ADD: {
        if (!kernel_open_file_supports_epoll(target_file)) {
            *linux_result = -KERNEL_EPERM;
            break;
        }
        /* 分配允许换任务；候选尚未发布，图与重复key必须在它之后检查。 */
        heap_status = kernel_heap_allocate_zeroed(epoll->heap, 1U, sizeof(*item), (void **)&item);
        if (heap_status != KERNEL_HEAP_STATUS_OK) {
            *linux_result = -KERNEL_ENOMEM;
            break;
        }
        item->heap = epoll->heap;
        item->references = 1;
        item->target_fd = (int)fd;
        item->target_file = target_file;
        item->epoll = epoll;
        item->events = events;
        item->data = data;
        kernel_wait_node_init_callback(&item->wait_node, kernel_epoll_wait_callback, item);
        kernel_preempt_disable();
        int error = 0;
        if (kernel_open_file_kind(target_file) == KERNEL_OPEN_FILE_KIND_EPOLL)
            error = epoll_check_nesting(epoll, target_file->epoll);
        if (!error && epoll_find_item(epoll, (int)fd, target_file)) error = -KERNEL_EEXIST;
        if (error) {
            kernel_preempt_enable();
            epoll_item_put(item);
            *linux_result = error;
            break;
        }
        EPOLL_STATE(irq);
        item->linked = 1;
        epoll_index_insert(epoll, item);
        item->target_next = target_file->ep_items;
        if (item->target_next) item->target_next->target_prev = item;
        target_file->ep_items = item;
        item->references++; /* query可睡眠，DEL只撤销登记引用。 */
        epoll_state_release(&irq);
        kernel_preempt_enable();
        target_queue = 0;
        current_revents = kernel_open_file_poll(target_file, events, &target_queue);
        if (kernel_open_file_kind(target_file) == KERNEL_OPEN_FILE_KIND_EPOLL)
            target_queue = &target_file->epoll->wait_queue;
        epoll_state_acquire(&irq);
        if (item->linked && target_queue) kernel_wait_queue_add(target_queue, &item->wait_node);
        uint64_t control = item->control_generation;
        uint32_t interest = item->events;
        epoll_state_release(&irq);
        /* 订阅前的通知可能已经结束；绑定后重新查询实际状态。 */
        if (item->linked) current_revents = kernel_open_file_poll(target_file, interest, 0);
        epoll_state_acquire(&irq);
        if (item->linked && control == item->control_generation &&
            (current_revents & (item->events | KERNEL_POLLERR | KERNEL_POLLHUP)))
            kernel_epoll_wait_callback(&item->wait_node, 0);
        epoll_state_release(&irq);
        epoll_item_put(item);
        *linux_result = 0;
        break;
    }
    case KERNEL_EPOLL_CTL_MOD: {
        EPOLL_STATE(irq);
        item = epoll_find_item(epoll, (int)fd, target_file);
        if (!item) {
            epoll_state_release(&irq);
            *linux_result = -KERNEL_ENOENT;
            break;
        }
        item->references++;
        uint64_t control = ++item->control_generation;
        item->events = events;
        item->data = data;
        item->oneshot_disarmed = 0;
        epoll_state_release(&irq);
        current_revents = kernel_open_file_poll(target_file, events, 0);
        epoll_state_acquire(&irq);
        if (item->linked && control == item->control_generation &&
            (current_revents & (events | KERNEL_POLLERR | KERNEL_POLLHUP)))
            kernel_epoll_wait_callback(&item->wait_node, 0);
        epoll_state_release(&irq);
        epoll_item_put(item);
        *linux_result = 0;
        break;
    }
    case KERNEL_EPOLL_CTL_DEL: {
        EPOLL_STATE(irq);
        item = epoll_find_item(epoll, (int)fd, target_file);
        if (item) item->references++;
        epoll_state_release(&irq);
        if (item) { epoll_item_unlink_and_destroy(item); epoll_item_put(item); }
        *linux_result = item ? 0 : -KERNEL_ENOENT;
        break;
    }
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
    if (request->target && kernel_open_file_release(&request->target) != KERNEL_OPEN_FILE_STATUS_OK)
        __builtin_trap();
    if (request->observing) {
        struct kernel_epoll_item *item = request->current;
        request->current = 0;
        if (item) epoll_item_put(item);
        return;
    }
    EPOLL_STATE(irq);
    if (epoll->scan_owner == request) {
        /* 尚未交付的项目仍由扫描引用保活；DEL只撤销注册引用。 */
        if (request->current) {
            struct kernel_epoll_item *item = request->current;
            request->current = 0;
            item->in_scan = 0;
            epoll_ready_add(epoll, item);
            epoll_state_release(&irq);
            epoll_item_put(item);
            epoll_state_acquire(&irq);
        }
        while (epoll->pending_head) {
            struct kernel_epoll_item *item = epoll->pending_head;
            epoll_remove_from_pending_list(epoll, item);
            epoll_ready_add(epoll, item);
        }
        epoll->scan_owner = 0;
        if (epoll->ready_head) epoll_notify(epoll);
    }
    epoll_state_release(&irq);
}

static void epoll_request_finish(struct kernel_epoll_wait_request *request)
{
    epoll_scan_finish(request);
    if (request->task) {
        if (kernel_task_epoll_clear(request->task, request, request->previous) != KERNEL_TASK_STATUS_OK) __builtin_trap();
        request->task = 0;
        request->previous = 0;
    }
    if (request->file && kernel_open_file_release(&request->file) != KERNEL_OPEN_FILE_STATUS_OK)
        __builtin_trap();
}

void kernel_epoll_abort_wait(struct kernel_epoll_wait_request *request)
{
    while (request) {
        struct kernel_epoll_wait_request *previous = request->previous;
        epoll_request_finish(request);
        request = previous;
    }
}

struct epoll_candidate {
    uint64_t control, notification;
    uint32_t interest;
    struct linux_epoll_event event;
};

static int epoll_scan_begin(struct kernel_epoll_wait_request *request)
{
    EPOLL_STATE(irq);
    struct kernel_epoll *epoll = request->epoll;
    int begin = !epoll->scan_owner && epoll->ready_head;
    if (begin) epoll->scan_owner = request;
    epoll_state_release(&irq);
    return begin;
}

/* poll和wait共用候选刷新；只跨可能睡眠的目标query持有item/OFD借用。 */
static int epoll_candidate_refresh(struct kernel_epoll_wait_request *request, struct epoll_candidate *candidate)
{
    struct kernel_epoll *epoll = request->epoll;
    EPOLL_STATE(irq);
    struct kernel_epoll_item *item = request->observing ? request->current : epoll->ready_head;
    if (!item) {
        epoll_state_release(&irq);
        return 0;
    }
    if (!request->observing) {
        epoll_remove_from_ready_list(epoll, item);
        if (item->references == UINT32_MAX || item->in_scan) __builtin_trap();
        item->references++;
        item->in_scan = 1;
        request->current = item;
    }
    *candidate = (struct epoll_candidate){.control = item->control_generation,
        .notification = item->notification_generation, .interest = item->events,
        .event.data = item->data};
    if (item->linked && !item->oneshot_disarmed && item->target_file) {
        struct kernel_open_file_description *target=item->target_file;
        if (kernel_open_file_acquire(target)==KERNEL_OPEN_FILE_STATUS_OK) request->target=target;
        else if (__atomic_load_n(&target->references,__ATOMIC_RELAXED)) __builtin_trap();
        /* 末引用可已归零；关闭者须取得此锁拆关联后才能释放存储。
         * 该窗口只跳过已退休目标，不复活OFD，也不查询其关闭中的后端。 */
    }
    epoll_state_release(&irq);
    if (request->target)
        candidate->event.events = kernel_open_file_poll(request->target, candidate->interest, 0) &
            (candidate->interest | KERNEL_POLLERR | KERNEL_POLLHUP);
    epoll_state_acquire(&irq);
    if (!item->linked || item->oneshot_disarmed || candidate->control != item->control_generation)
        candidate->event.events = 0;
    epoll_state_release(&irq);
    return 1;
}

static int epoll_candidate_finish(struct kernel_epoll_wait_request *request,
    const struct epoll_candidate *candidate, int delivered, int preserve)
{
    EPOLL_STATE(irq);
    struct kernel_epoll_item *item = request->current;
    struct kernel_epoll *epoll = request->epoll;
    int pruned = 0;
    if (request->observing) {
        if (!candidate->event.events && item->linked && !item->in_scan &&
            candidate->control == item->control_generation &&
            candidate->notification == item->notification_generation) {
            epoll_remove_from_ready_list(epoll, item);
            epoll_remove_from_pending_list(epoll, item);
            pruned = 1;
        }
    } else {
        if (item->linked && candidate->control == item->control_generation) {
            if (delivered && (candidate->interest & KERNEL_EPOLLONESHOT)) item->oneshot_disarmed = 1;
            else if (preserve || (delivered && !(candidate->interest & KERNEL_EPOLLET)))
                epoll_pending_add(epoll, item);
        }
        if (item->linked && (candidate->control != item->control_generation ||
                            candidate->notification != item->notification_generation))
            epoll_pending_add(epoll, item);
    }
    epoll_state_release(&irq);
    /* 最后OFD关闭可能执行后端操作，不能在IRQ或协议scope内执行。 */
    if (request->target && kernel_open_file_release(&request->target) != KERNEL_OPEN_FILE_STATUS_OK) __builtin_trap();
    epoll_state_acquire(&irq);
    request->current = 0;
    if (!request->observing) item->in_scan = 0;
    epoll_state_release(&irq);
    epoll_item_put(item);
    return pruned;
}

int kernel_epoll_wait_pending(struct kernel_open_file_description *file, struct kernel_wait_queue **out_queue)
{
    struct kernel_epoll *epoll = file->epoll;
    EPOLL_STATE(irq);
    if (out_queue) *out_queue = &epoll->wait_queue;
    int pending = !epoll->scan_owner && epoll->ready_head;
    epoll_state_release(&irq);
    return pending;
}

/* 纯查询不占交付游标。常见空结果直接摘ready头；并发交付或代次变化时，
 * 本次按key向前借用候选，不跨目标query保留AVL节点或另一调用者的扫描指针。 */
struct epoll_observe_cursor {
    int indexed, started, fd, skip_fd, skip;
    uintptr_t file, skip_file;
};
static void epoll_observe_indexed(struct epoll_observe_cursor *cursor)
{
    if (cursor->indexed) return;
    cursor->skip = cursor->started;
    cursor->skip_fd = cursor->fd;
    cursor->skip_file = cursor->file;
    cursor->indexed = 1;
    cursor->started = 0;
}
static int epoll_observe_next(struct kernel_epoll_wait_request *request,
    struct epoll_observe_cursor *cursor)
{
    struct kernel_epoll *epoll = request->epoll;
    for (;;) {
        EPOLL_STATE(irq);
        if (epoll->scan_owner || epoll->pending_head) epoll_observe_indexed(cursor);
        struct kernel_epoll_item *item = epoll->ready_head;
        if (cursor->indexed) {
            item = epoll->items_root;
            struct kernel_epoll_item *next = 0;
            while (item) {
                if (!cursor->started || epoll_index_compare(cursor->fd,
                    (struct kernel_open_file_description *)cursor->file, item) < 0) {
                    next = item; item = item->index_left;
                } else item = item->index_right;
            }
            item = next;
        }
        if (!item) { epoll_state_release(&irq); return 0; }
        cursor->fd = item->target_fd; cursor->file = (uintptr_t)item->target_file; cursor->started = 1;
        int candidate = !(cursor->skip && cursor->fd == cursor->skip_fd && cursor->file == cursor->skip_file) &&
            item->linked && !item->oneshot_disarmed &&
            (item->on_ready_list || item->on_pending_list || item->in_scan);
        if (candidate) {
            if (item->references == UINT32_MAX) __builtin_trap();
            item->references++;
            request->current = item;
        }
        epoll_state_release(&irq);
        if (candidate) return 1;
        /* 非候选不借用指针，也不在一次IRQ段里遍历全部登记。 */
        epoll_observe_indexed(cursor);
    }
}

uint32_t kernel_epoll_poll(struct kernel_epoll *epoll, uint32_t requested_events,
    struct kernel_wait_queue **out_queue)
{
    if (out_queue) *out_queue = epoll ? &epoll->wait_queue : 0;
    if (!epoll) return KERNEL_POLLNVAL;
    struct kernel_epoll_wait_request request = {.epoll = epoll, .task = kernel_task_current(), .observing = 1};
    if (request.task) {
        request.file = epoll->file;
        if (request.file && kernel_open_file_acquire(request.file) != KERNEL_OPEN_FILE_STATUS_OK) __builtin_trap();
        if (kernel_task_epoll_register(request.task, &request, &request.previous) != KERNEL_TASK_STATUS_OK) __builtin_trap();
    }
    struct epoll_candidate candidate;
    uint32_t revents = 0;
    struct epoll_observe_cursor cursor = {0};
    while (epoll_observe_next(&request, &cursor)) {
        if (!epoll_candidate_refresh(&request, &candidate)) __builtin_trap();
        int ready = candidate.event.events != 0;
        int pruned = epoll_candidate_finish(&request, &candidate, 0, ready);
        if (ready) { revents = KERNEL_POLLIN | KERNEL_POLLRDNORM; break; }
        if (!pruned) epoll_observe_indexed(&cursor);
    }
    epoll_request_finish(&request);
    return revents & requested_events;
}

static int64_t epoll_deliver(struct kernel_epoll_wait_request *request,
    struct kernel_mm *mm, uint64_t user_events, size_t maximum)
{
    if (!epoll_scan_begin(request)) return 0;
    int64_t delivered = 0;
    struct epoll_candidate candidate;
    while ((size_t)delivered < maximum && epoll_candidate_refresh(request, &candidate)) {
        int copied_event = 0;
        if (candidate.event.events) {
            size_t copied = 0;
            enum kernel_uaccess_status status = kernel_copy_to_user(mm,
                user_events + (uint64_t)delivered * sizeof(candidate.event),
                &candidate.event, sizeof(candidate.event), &copied);
            if (status != KERNEL_UACCESS_STATUS_OK || copied != sizeof(candidate.event)) {
                if (status != KERNEL_UACCESS_STATUS_OK && status != KERNEL_UACCESS_STATUS_FAULT) __builtin_trap();
                if (!delivered) delivered = -KERNEL_EFAULT;
                break;
            }
            delivered++;
            copied_event = 1;
        }
        /* 只有完整用户复制提交ET/ONESHOT；旧控制代次不能覆盖MOD。 */
        epoll_candidate_finish(request, &candidate, copied_event, 0);
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
    if (kernel_task_epoll_register(task, &request, &request.previous) != KERNEL_TASK_STATUS_OK) __builtin_trap();

    sig_error = epoll_setup_sigmask(mm, task, user_sigmask, sigsetsize, &saved_mask, &mask_modified);
    if (sig_error != 0) {
        epoll_request_finish(&request);
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
            EPOLL_STATE(saved_intr);
            uint64_t sleep_deadline = deadline;
            struct kernel_wait_token token;
            enum kernel_scheduler_status sched_status = kernel_wait_prepare(
                &epoll->wait_queue, sleep_deadline, 1, &token);
            if (sched_status == KERNEL_SCHEDULER_STATUS_OK) {
                int ready = !epoll->scan_owner && epoll->ready_head;
                uintptr_t interrupts=epoll_state_unlock_wait(&saved_intr);
                if (!ready) sched_status = kernel_wait_park(&token, &wake_reason);
                if (kernel_wait_finish(&token) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
                arch_interrupt_restore(interrupts);
                if (ready) continue;
            }
            epoll_state_release(&saved_intr);
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
    epoll_request_finish(&request);
    return KERNEL_FILES_STATUS_OK;
}
