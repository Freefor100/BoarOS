#include "private.h"

#include <kernel/errno.h>
#include <kernel/futex.h>
#include <kernel/mm.h>
#include <kernel/time.h>
#include <kernel/uaccess.h>

#define FUTEX_BUCKETS 256U
#define FUTEX_PRIVATE 128U
#define FUTEX_WAITERS UINT32_C(0x80000000)
#define FUTEX_OWNER_DIED UINT32_C(0x40000000)
#define FUTEX_TID_MASK UINT32_C(0x3fffffff)
#define ROBUST_LIST_LIMIT 2048U

static struct kernel_wait_queue buckets[FUTEX_BUCKETS];

static int shared_anon_futex_unsupported(struct kernel_task *task,
                                          uint64_t address,
                                          uint32_t operation)
{
    struct kernel_vma vma;

    return (operation & FUTEX_PRIVATE) == 0U &&
           kernel_mm_vma_lookup(&task->mm, address, &vma) ==
               KERNEL_MM_STATUS_OK &&
           vma.kind == KERNEL_VMA_KIND_ANON_SHARED;
}

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

static int64_t futex_wait_until(struct kernel_task *task, uint64_t address,
                                uint32_t operation, uint32_t value,
                                int has_timeout, uint64_t deadline_ns,
                                enum kernel_scheduler_status *status)
{
    uint32_t actual;
    uint64_t deadline = 0U;
    int expired = 0;
    size_t copied = 0U;
    enum kernel_wait_wake_reason reason;
    enum kernel_uaccess_status access;

    if (has_timeout) {
        enum kernel_time_status time_status =
            kernel_time_deadline_from_monotonic(deadline_ns, &deadline);

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
    if (reason == KERNEL_WAIT_TIMEOUT) return -KERNEL_ETIMEDOUT;
    if (reason == KERNEL_WAIT_SIGNALLED) {
        if (has_timeout) {
            kernel_signal_note_futex_timed_restart(task, address, operation,
                                                   value, deadline_ns);
        }
        return -KERNEL_ERESTARTSYS;
    }
    return 0;
}

int64_t kernel_futex(struct kernel_task *task, uint64_t address,
                     uint32_t operation, uint32_t value,
                     uint64_t timeout_or_count, uint64_t address2,
                     enum kernel_scheduler_status *status)
{
    uint32_t command = operation & ~FUTEX_PRIVATE;
    uint64_t deadline_ns = 0U;
    size_t copied = 0U;
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
    if (shared_anon_futex_unsupported(task, address, operation))
        return -KERNEL_ENOTSUP;
    if (command == 1U || command == 3U) {
        if ((int32_t)value < 0) return -KERNEL_EINVAL;
        if (command == 3U && ((address2 & 3U) != 0U ||
            address == address2 || timeout_or_count > INT32_MAX))
            return -KERNEL_EINVAL;
        if (command == 3U && kernel_user_range_check(address2,
            sizeof(uint32_t)) != KERNEL_UACCESS_STATUS_OK)
            return -KERNEL_EFAULT;
        if (command == 3U &&
            shared_anon_futex_unsupported(task, address2, operation))
            return -KERNEL_ENOTSUP;
        return futex_wake(task->mm.record_page_address, address, value,
                          command == 3U ? (uint32_t)timeout_or_count : 0U,
                          address2);
    }
    if (timeout_or_count != 0U) {
        struct { int64_t seconds, nanoseconds; } duration;
        uint64_t now;
        __uint128_t delta;

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
        deadline_ns = now >= INT64_MAX ||
                      delta > (uint64_t)INT64_MAX - now
                          ? INT64_MAX
                          : now + (uint64_t)delta;
    }
    return futex_wait_until(task, address, operation, value,
                            timeout_or_count != 0U, deadline_ns, status);
}

int64_t kernel_futex_restart_timed(
    struct kernel_task *task, uint64_t address, uint32_t operation,
    uint32_t value, uint64_t deadline_ns,
    enum kernel_scheduler_status *status)
{
    uint32_t command = operation & ~FUTEX_PRIVATE;

    *status = KERNEL_SCHEDULER_STATUS_OK;
    if (task != scheduler.current || riscv_interrupt_is_enabled()) {
        *status = KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        return 0;
    }
    if (command != 0U || (address & 3U) != 0U ||
        kernel_user_range_check(address, sizeof(uint32_t)) !=
            KERNEL_UACCESS_STATUS_OK) {
        *status = KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        return 0;
    }
    if (shared_anon_futex_unsupported(task, address, operation))
        return -KERNEL_ENOTSUP;
    return futex_wait_until(task, address, operation, value, 1,
                            deadline_ns, status);
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

void kernel_futex_set_robust_list(struct kernel_task *task, uint64_t head)
{
    task->robust_list_head = head;
}

uint64_t kernel_futex_get_robust_list(const struct kernel_task *task)
{
    return task->robust_list_head;
}

static int robust_read_u64(struct kernel_mm *mm, uint64_t address,
                           uint64_t *value)
{
    size_t copied = 0U;

    return kernel_copy_from_user(mm, value, address, sizeof(*value),
                                 &copied) == KERNEL_UACCESS_STATUS_OK &&
           copied == sizeof(*value);
}

static int robust_word_address(uint64_t entry, int64_t offset,
                               uint64_t *address)
{
    __int128 sum = (__int128)entry + (__int128)offset;

    if (sum < 0 || sum > UINT64_MAX) return 0;
    *address = (uint64_t)sum;
    return (*address & 3U) == 0U &&
           kernel_user_range_check(*address, sizeof(uint32_t)) ==
               KERNEL_UACCESS_STATUS_OK;
}

static int robust_release_word(struct kernel_task *task, uint64_t entry,
                               int64_t offset, uint32_t owner_tid,
                               int pending)
{
    uint64_t address;
    uint32_t value, observed;
    size_t copied = 0U;

    if (!robust_word_address(entry, offset, &address)) return 0;
    for (unsigned attempt = 0U; attempt < 32U; attempt++) {
        copied = 0U;
        if (kernel_copy_from_user(&task->mm, &value, address,
                                  sizeof(value), &copied) !=
                KERNEL_UACCESS_STATUS_OK ||
            copied != sizeof(value)) return 0;
        if ((value & FUTEX_TID_MASK) != owner_tid) {
            if (pending &&
                ((value & FUTEX_TID_MASK) == 0U ||
                 (value & FUTEX_WAITERS) == 0U))
                (void)futex_wake(task->mm.record_page_address, address,
                                 1U, 0U, 0U);
            return 1;
        }
        if (kernel_user_cmpxchg_u32(
                &task->mm, address, value,
                (value & FUTEX_WAITERS) | FUTEX_OWNER_DIED,
                &observed) != KERNEL_UACCESS_STATUS_OK)
            return 0;
        if (observed != value) continue;
        if ((value & FUTEX_WAITERS) != 0U)
            (void)futex_wake(task->mm.record_page_address, address,
                             1U, 0U, 0U);
        return 1;
    }
    return 0;
}

void kernel_futex_release_robust(struct kernel_task *task, int32_t owner_tid)
{
    uint64_t head = task->robust_list_head;
    uint64_t next, pending, offset_bits;
    uint64_t entry, pending_entry;
    uint32_t limit = ROBUST_LIST_LIMIT;

    task->robust_list_head = 0U;
    if (head == 0U || task->mm.state != KERNEL_MM_LIVE ||
        kernel_user_range_check(head, 3U * sizeof(uint64_t)) !=
            KERNEL_UACCESS_STATUS_OK)
        return;
    if (!robust_read_u64(&task->mm, head, &entry) ||
        !robust_read_u64(&task->mm, head + sizeof(uint64_t),
                         &offset_bits) ||
        !robust_read_u64(&task->mm, head + 2U * sizeof(uint64_t),
                         &pending))
        return;
    pending_entry = pending & ~UINT64_C(1);
    while ((entry & ~UINT64_C(1)) != head && limit-- != 0U) {
        uint64_t current = entry & ~UINT64_C(1);

        /* Read the next link before a word update can wake another thread. */
        if (!robust_read_u64(&task->mm, current, &next)) return;
        if (current != pending_entry && (entry & 1U) == 0U &&
            !robust_release_word(task, current, (int64_t)offset_bits,
                                 (uint32_t)owner_tid, 0))
            return;
        entry = next;
    }
    if (pending_entry != 0U && (pending & 1U) == 0U)
        (void)robust_release_word(task, pending_entry,
                                  (int64_t)offset_bits,
                                  (uint32_t)owner_tid, 1);
}
