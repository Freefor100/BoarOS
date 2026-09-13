#include "private.h"

#include <kernel/errno.h>
#include <kernel/futex.h>
#include <kernel/time.h>
#include <kernel/uaccess.h>

#define FUTEX_BUCKETS 256U
#define FUTEX_PRIVATE 128U

static struct kernel_wait_queue buckets[FUTEX_BUCKETS];

static struct kernel_wait_queue *futex_bucket(uint64_t mm, uint64_t address)
{
    uint64_t hash = (mm >> 12) ^ (address >> 2) ^ (address >> 12);
    struct kernel_wait_queue *queue = &buckets[hash & (FUTEX_BUCKETS - 1U)];

    if (queue->initialized == 0U) kernel_wait_queue_init(queue);
    return queue;
}

static int64_t futex_wake(uint64_t mm, uint64_t address, uint32_t count,
                          uint32_t requeue, uint64_t address2)
{
    struct kernel_wait_queue *queue = futex_bucket(mm, address);
    struct kernel_wait_node *node = queue->head;
    struct kernel_wait_node *last = queue->tail;
    uint32_t woken = 0U, moved = 0U;

    while (node != 0 && (woken < count || moved < requeue)) {
        struct kernel_wait_node *next = node->next;
        struct kernel_task *task = node->task;

        if (task != 0 && task->futex_mm == mm && task->futex_address == address) {
            if (woken < count) {
                blocked_unlink(task);
                scheduler_wake_task(task, KERNEL_WAIT_WOKEN);
                woken++;
            } else {
                scheduler_wait_requeue(task, futex_bucket(mm, address2));
                task->futex_address = address2;
                moved++;
            }
        }
        if (node == last) break;
        node = next;
    }
    return (int64_t)woken + moved;
}

int64_t kernel_futex(struct kernel_task *task, uint64_t address,
                     uint32_t operation, uint32_t value,
                     uint64_t timeout_or_count, uint64_t address2,
                     enum kernel_scheduler_status *status)
{
    uint32_t command = operation & ~FUTEX_PRIVATE;
    uint32_t actual;
    uint64_t deadline = 0U;
    int expired = 0;
    size_t copied = 0U;
    enum kernel_wait_wake_reason reason;
    enum kernel_uaccess_status access;

    *status = KERNEL_SCHEDULER_STATUS_OK;
    if (task != scheduler.current || riscv_interrupt_is_enabled()) {
        *status = KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        return 0;
    }
    if (command != 0U && command != 1U && command != 3U)
        return -KERNEL_ENOSYS;
    if ((address & 3U) != 0U) return -KERNEL_EINVAL;
    if (kernel_user_range_check(address, sizeof(uint32_t)) !=
        KERNEL_UACCESS_STATUS_OK) return -KERNEL_EFAULT;
    if (command == 1U || command == 3U) {
        if ((int32_t)value < 0) return -KERNEL_EINVAL;
        if (command == 3U && ((address2 & 3U) != 0U ||
            address == address2 || timeout_or_count > INT32_MAX))
            return -KERNEL_EINVAL;
        if (command == 3U && kernel_user_range_check(address2,
            sizeof(uint32_t)) != KERNEL_UACCESS_STATUS_OK)
            return -KERNEL_EFAULT;
        return futex_wake(task->mm.record_page_address, address, value,
                          command == 3U ? (uint32_t)timeout_or_count : 0U,
                          address2);
    }
    if (timeout_or_count != 0U) {
        struct { int64_t seconds, nanoseconds; } duration;
        uint64_t now, target;
        __uint128_t delta;
        enum kernel_time_status time_status;

        access = kernel_copy_from_user(&task->mm, &duration,
                                        timeout_or_count, sizeof(duration),
                                        &copied);
        if (access != KERNEL_UACCESS_STATUS_OK &&
            access != KERNEL_UACCESS_STATUS_FAULT) {
            *status = KERNEL_SCHEDULER_STATUS_INVALID_STATE;
            return 0;
        }
        if (access != KERNEL_UACCESS_STATUS_OK || copied != sizeof(duration))
            return -KERNEL_EFAULT;
        if (duration.seconds < 0 || duration.nanoseconds < 0 ||
            duration.nanoseconds >= 1000000000) return -KERNEL_EINVAL;
        delta = (__uint128_t)(uint64_t)duration.seconds * 1000000000U +
                (uint64_t)duration.nanoseconds;
        now = kernel_time_monotonic_ns();
        target = now >= INT64_MAX || delta > (uint64_t)INT64_MAX - now ? INT64_MAX :
                 now + (uint64_t)delta;
        time_status = kernel_time_deadline_from_monotonic(target, &deadline);
        if (time_status == KERNEL_TIME_STATUS_DEADLINE_PASSED) expired = 1;
        else if (time_status != KERNEL_TIME_STATUS_OK) {
            *status = KERNEL_SCHEDULER_STATUS_INVALID_STATE;
            return 0;
        }
    }
    access = kernel_copy_from_user(&task->mm, &actual, address,
                                   sizeof(actual), &copied);
    if (access != KERNEL_UACCESS_STATUS_OK &&
        access != KERNEL_UACCESS_STATUS_FAULT) {
        *status = KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        return 0;
    }
    if (access != KERNEL_UACCESS_STATUS_OK || copied != sizeof(actual))
        return -KERNEL_EFAULT;
    if (actual != value) return -KERNEL_EAGAIN;
    if (expired) return -KERNEL_ETIMEDOUT;
    /* SIE stays clear from comparison through enqueue and context switch.
     * No other user thread can change the word between these operations. */
    task->futex_mm = task->mm.record_page_address;
    task->futex_address = address;
    *status = kernel_scheduler_block_current(
        futex_bucket(task->futex_mm, address), deadline, 1, &reason);
    task->futex_mm = 0U;
    task->futex_address = 0U;
    if (*status != KERNEL_SCHEDULER_STATUS_OK) return 0;
    return reason == KERNEL_WAIT_TIMEOUT ? -KERNEL_ETIMEDOUT :
           reason == KERNEL_WAIT_SIGNALLED ? -KERNEL_EINTR : 0;
}

void kernel_futex_clear_tid(struct kernel_task *task)
{
    uint64_t address = task->clear_tid_address;
    uint32_t zero = 0U;
    size_t copied;

    task->clear_tid_address = 0U;
    if (address == 0U || task->mm.state != KERNEL_MM_LIVE) return;
    (void)kernel_copy_to_user(&task->mm, address, &zero, sizeof(zero), &copied);
    (void)futex_wake(task->mm.record_page_address, address, 1U, 0U, 0U);
}
