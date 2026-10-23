#include <kernel/scheduler.h>
#include "private.h"

#include <kernel/errno.h>
#include <kernel/futex.h>
#include <kernel/mm.h>
#include <kernel/page.h>
#include <kernel/memory_object.h>
#include <kernel/time.h>
#include <kernel/uaccess.h>
#include <kernel/vfs.h>

#define FUTEX_BUCKETS 256U
#define FUTEX_PRIVATE 128U
#define FUTEX_CLOCK_REALTIME 256U
#define FUTEX_WAIT_BITSET 9U
#define FUTEX_WAKE_BITSET 10U
#define FUTEX_BITSET_MATCH_ANY UINT32_MAX
#define FUTEX_WAITERS UINT32_C(0x80000000)
#define FUTEX_OWNER_DIED UINT32_C(0x40000000)
#define FUTEX_TID_MASK UINT32_C(0x3fffffff)
#define ROBUST_LIST_LIMIT 2048U

struct futex_bucket {
    struct kernel_mutex operation;
    struct kernel_wait_queue waiters;
};
static struct futex_bucket buckets[FUTEX_BUCKETS];

static void futex_key_release(struct kernel_futex_key *key)
{
    if (key->kind == KERNEL_FUTEX_KEY_SHARED_MEMORY)
        kernel_memory_object_release(&key->shared_object);
    else if (key->kind == KERNEL_FUTEX_KEY_SHARED_FILE &&
             kernel_vfs_node_release(&key->file_node)) __builtin_trap();
    *key = (struct kernel_futex_key){0};
}

static void futex_key_hold(const struct kernel_futex_key *key)
{
    if (key->kind == KERNEL_FUTEX_KEY_SHARED_MEMORY &&
        kernel_memory_object_acquire(key->shared_object)) __builtin_trap();
    if (key->kind == KERNEL_FUTEX_KEY_SHARED_FILE &&
        kernel_vfs_node_acquire(key->file_node)) __builtin_trap();
}

static int64_t futex_prepare(struct kernel_task *task,
    const uint64_t *addresses, unsigned count, uint32_t operation,
    unsigned pin_index, uint32_t access, struct kernel_futex_key *keys,
    struct kernel_mm_user_pin *pin, enum kernel_scheduler_status *status)
{
    enum kernel_mm_status result = kernel_mm_prepare_futex(&task->mm,
        addresses, count, (operation & FUTEX_PRIVATE) == 0, pin_index,
        access, keys, pin);
    if (result == KERNEL_MM_STATUS_INVALID_ARGUMENT) return -KERNEL_EINVAL;
    if (result == KERNEL_MM_STATUS_NO_MEMORY) return -KERNEL_ENOMEM;
    if (result == KERNEL_MM_STATUS_NOT_MAPPED ||
        result == KERNEL_MM_STATUS_ACCESS ||
        result == KERNEL_MM_STATUS_ADDRESS_SPACE ||
        result == KERNEL_MM_STATUS_BUS_FAULT) return -KERNEL_EFAULT;
    if (result != KERNEL_MM_STATUS_OK)
        *status = KERNEL_SCHEDULER_STATUS_INVALID_STATE;
    return 0;
}

static int futex_key_equal(const struct kernel_futex_key *left,
                           const struct kernel_futex_key *right)
{
    return left->kind == right->kind &&
           left->identity == right->identity && left->offset == right->offset;
}

static struct futex_bucket *futex_bucket(const struct kernel_futex_key *key)
{
    uint64_t hash = (key->identity >> 4U) ^ key->identity ^
                    (key->offset >> 2U) ^ (key->offset >> 12U) ^ key->kind;
    unsigned index = hash & (FUTEX_BUCKETS - 1U);
    return &buckets[index];
}

static void futex_lock_buckets(const struct kernel_futex_key *keys,
    unsigned count, struct futex_bucket **resolved, struct kernel_lock_guard *guards)
{
    struct futex_bucket *first = resolved[0] = futex_bucket(&keys[0]);
    struct futex_bucket *second = resolved[1] = count == 2 ? futex_bucket(&keys[1]) : first;
    { KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
      for (unsigned i = 0; i < count; i++) {
          struct futex_bucket *bucket = resolved[i];
          if (!bucket->waiters.initialized) {
              kernel_wait_queue_init(&bucket->waiters);
              kernel_mutex_init(&bucket->operation, 26, (uintptr_t)(bucket - buckets) + 1U);
          }
      } }
    if (first > second) {
        struct futex_bucket *swap = first; first = second; second = swap;
    }
    kernel_mutex_lock(&first->operation, &guards[0]);
    if (first != second) kernel_mutex_lock(&second->operation, &guards[1]);
}

static void futex_unlock_buckets(struct kernel_lock_guard *guards)
{
    if (guards[1].lock) kernel_lock_release(&guards[1]);
    kernel_lock_release(&guards[0]);
}

/* 桶操作锁跨扫描保留比较/登记/迁移的次序，raw只保护一个节点。
 * moved个源后备引用由caller在解桶锁、归还用户pin之后释放。 */
static int64_t futex_wake(const struct kernel_futex_key *source,
    uint32_t count, uint32_t requeue, const struct kernel_futex_key *target,
    uint32_t bitset, uint32_t *moved_count, struct futex_bucket *bucket,
    struct futex_bucket *target_bucket)
{
    struct kernel_wait_queue *queue = &bucket->waiters;
    struct kernel_wait_queue *destination = target_bucket ? &target_bucket->waiters : 0;
    struct kernel_wait_cursor cursor;
    uint32_t woken = 0, moved = 0;
    kernel_wait_cursor_begin(queue, &cursor);
    while (cursor.node && (woken < count || moved < requeue)) {
        struct kernel_wait_node *node = cursor.node;
        struct kernel_task *task = node->task;
        int move = 0;
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            if (task && node->queue == queue && futex_key_equal(&task->futex_key, source) &&
                kernel_wait_record_of(task)->phase != KERNEL_WAIT_NOTIFIED) {
                if (woken < count && (task->futex_bitset & bitset)) {
                    struct kernel_wait_token token = {task, node->generation};
                    if (kernel_wait_notify_locked(&token, KERNEL_WAIT_WOKEN)) woken++;
                } else move = moved < requeue;
            }
        }
        if (move) {
            /* 当前node借用覆盖task_pin，放游标后task借用阻止finish回收。
             * 同桶扫描由operation串行；此时没有其他游标持有要移动的node。 */
            kernel_wait_task_pin(task);
            kernel_wait_cursor_advance(&cursor);
            {
                KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
                if (node->queue == queue && futex_key_equal(&task->futex_key, source)) {
                    enum kernel_scheduler_status result =
                        kernel_wait_node_move_locked(node, destination);
                    if (result == KERNEL_SCHEDULER_STATUS_OK) {
                        futex_key_hold(target);
                        task->futex_key = *target;
                        moved++;
                    } else if (result != KERNEL_SCHEDULER_STATUS_EMPTY) __builtin_trap();
                }
            }
            kernel_wait_task_unpin(task);
            continue;
        }
        kernel_wait_cursor_advance(&cursor);
    }
    kernel_wait_cursor_end(&cursor);
    if (moved_count) *moved_count = moved;
    return (int64_t)woken + moved;
}

static uint32_t futex_wake_count(uint32_t count)
{
    /* legacy WAKE/WAKE_OP先唤醒再比较signed count，非正数仍可唤醒一个。 */
    return (int32_t)count <= 0 ? 1U : count;
}

static int futex_atomic_op(uint32_t *word, uint32_t encoded)
{
    unsigned op = (encoded >> 28) & 7U, comparison = (encoded >> 24) & 15U;
    int32_t argument = (int32_t)(encoded << 8) >> 20;
    int32_t compare_argument = (int32_t)(encoded << 20) >> 20;
    uint32_t operand = (uint32_t)argument, old;
    if (encoded & UINT32_C(0x80000000)) operand = UINT32_C(1) << (operand & 31U);
    switch (op) {
    case 0: old = __atomic_exchange_n(word, operand, __ATOMIC_SEQ_CST); break;
    case 1: old = __atomic_fetch_add(word, operand, __ATOMIC_SEQ_CST); break;
    case 2: old = __atomic_fetch_or(word, operand, __ATOMIC_SEQ_CST); break;
    case 3: old = __atomic_fetch_and(word, ~operand, __ATOMIC_SEQ_CST); break;
    case 4: old = __atomic_fetch_xor(word, operand, __ATOMIC_SEQ_CST); break;
    default: return -KERNEL_ENOSYS;
    }
    /* 未知comparison仍保留已经完成的原子更新，与Linux错误顺序一致。 */
    switch (comparison) {
    case 0: return (int32_t)old == compare_argument;
    case 1: return (int32_t)old != compare_argument;
    case 2: return (int32_t)old < compare_argument;
    case 3: return (int32_t)old <= compare_argument;
    case 4: return (int32_t)old > compare_argument;
    case 5: return (int32_t)old >= compare_argument;
    default: return -KERNEL_ENOSYS;
    }
}

static int64_t futex_wait_until(struct kernel_task *task, uint64_t address,
    uint32_t operation, uint32_t value, int has_timeout, uint64_t deadline_ns,
    uint32_t bitset, enum kernel_scheduler_status *status)
{
    struct kernel_futex_key key;
    struct kernel_mm_user_pin pin = {0};
    struct kernel_lock_guard guards[2] = {0};
    struct futex_bucket *resolved[2];
    uint64_t deadline = 0;
    int expired = 0;
    if (has_timeout) {
        enum kernel_time_status result = kernel_time_deadline_from_monotonic(deadline_ns, &deadline);
        if (result == KERNEL_TIME_STATUS_DEADLINE_PASSED) expired = 1;
        else if (result != KERNEL_TIME_STATUS_OK) {
            *status = KERNEL_SCHEDULER_STATUS_INVALID_STATE;
            return 0;
        }
    }
    int64_t result = futex_prepare(task, &address, 1, operation, 0,
                                    KERNEL_MM_READ, &key, &pin, status);
    if (*status != KERNEL_SCHEDULER_STATUS_OK || result) return result;
    futex_lock_buckets(&key, 1, resolved, guards);
    uint32_t *word = (uint32_t *)((unsigned char *)pin.page + (address & BOAROS_PAGE_MASK));
    uint32_t actual = __atomic_load_n(word, __ATOMIC_ACQUIRE);
    if (actual != value || expired) {
        futex_unlock_buckets(guards);
        kernel_mm_unpin_user_page(&pin);
        futex_key_release(&key);
        return actual != value ? -KERNEL_EAGAIN : -KERNEL_ETIMEDOUT;
    }
    task->futex_key = key;
    task->futex_bitset = bitset;
    struct kernel_wait_token token;
    *status = kernel_wait_prepare(&resolved[0]->waiters, deadline, 1, &token);
    /* token登记后不再取得任何可睡眠锁；先放桶锁再归还pin，MM编辑可排空它。 */
    futex_unlock_buckets(guards);
    kernel_mm_unpin_user_page(&pin);
    if (*status != KERNEL_SCHEDULER_STATUS_OK) {
        futex_key_release(&task->futex_key); task->futex_bitset = 0;
        return 0;
    }
    enum kernel_wait_wake_reason reason;
    *status = kernel_wait_park(&token, &reason);
    if (kernel_wait_finish(&token) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
    futex_key_release(&task->futex_key); task->futex_bitset = 0;
    if (*status != KERNEL_SCHEDULER_STATUS_OK) return 0;
    if (reason == KERNEL_WAIT_TIMEOUT) return -KERNEL_ETIMEDOUT;
    if (reason == KERNEL_WAIT_SIGNALLED) {
        if (has_timeout) kernel_signal_note_futex_timed_restart(task, address,
            operation, value, deadline_ns, bitset);
        return -KERNEL_ERESTARTSYS;
    }
    return 0;
}

int64_t kernel_futex(struct kernel_task *task, uint64_t address,
                     uint32_t operation, uint32_t value,
                     uint64_t timeout_or_count, uint64_t address2,
                     uint32_t bitset,
                     enum kernel_scheduler_status *status)
{
    uint32_t command = operation & ~(FUTEX_PRIVATE | FUTEX_CLOCK_REALTIME);
    uint64_t deadline_ns = 0U;
    int64_t result;
    size_t copied = 0U;
    enum kernel_uaccess_status access;

    *status = KERNEL_SCHEDULER_STATUS_OK;
    if (task != kernel_cpu_current()->current || arch_interrupt_is_enabled()) {
        *status = KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        return 0;
    }
    if (command != 0U && command != 1U && command != 3U &&
        command != 4U && command != 5U &&
        command != FUTEX_WAIT_BITSET && command != FUTEX_WAKE_BITSET)
        return -KERNEL_ENOSYS;
    if ((command == 0U || command == FUTEX_WAIT_BITSET) &&
        timeout_or_count != 0U) {
        struct { int64_t seconds, nanoseconds; } duration;
        uint64_t now;
        __uint128_t nanoseconds;

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
        nanoseconds = (__uint128_t)(uint64_t)duration.seconds * 1000000000U +
                (uint64_t)duration.nanoseconds;
        if (nanoseconds > INT64_MAX) nanoseconds = INT64_MAX;
        if (command == FUTEX_WAIT_BITSET) {
            uint64_t absolute = (uint64_t)nanoseconds;

            if ((operation & FUTEX_CLOCK_REALTIME) != 0U) {
                uint64_t offset = kernel_time_boot_realtime_offset();
                deadline_ns = absolute <= offset ? 0U : absolute - offset;
            } else deadline_ns = absolute;
        } else {
            now = kernel_time_monotonic_ns();
            deadline_ns = now >= INT64_MAX ||
                          nanoseconds > (uint64_t)INT64_MAX - now
                              ? INT64_MAX
                              : now + (uint64_t)nanoseconds;
        }
    }
    if ((operation & FUTEX_CLOCK_REALTIME) != 0U &&
        command != FUTEX_WAIT_BITSET) return -KERNEL_ENOSYS;
    if ((command == FUTEX_WAIT_BITSET || command == FUTEX_WAKE_BITSET) &&
        bitset == 0U) return -KERNEL_EINVAL;
    int requeue = command == 3U || command == 4U;
    uint32_t second_count = (uint32_t)timeout_or_count;
    if (requeue && ((int32_t)value < 0 || (int32_t)second_count < 0))
        return -KERNEL_EINVAL;
    if ((address & 3U) != 0U) return -KERNEL_EINVAL;
    if (kernel_user_range_check(address, sizeof(uint32_t)) != KERNEL_UACCESS_STATUS_OK)
        return -KERNEL_EFAULT;
    if (command == 1U || requeue || command == 5U || command == FUTEX_WAKE_BITSET) {
        uint64_t addresses[2] = {address, address2};
        unsigned count = requeue || command == 5U ? 2 : 1;
        struct kernel_futex_key keys[2];
        struct kernel_mm_user_pin pin = {0};
        struct kernel_lock_guard guards[2] = {0};
        struct futex_bucket *resolved[2];
        uint32_t pin_access = command == 4U ? KERNEL_MM_READ :
                              command == 5U ? KERNEL_MM_WRITE : 0;
        /* private key不访问映射；未知op先报ENOSYS，不用无关fault覆盖它。 */
        if (command == 5U && ((bitset >> 28) & 7U) > 4U && (operation & FUTEX_PRIVATE))
            pin_access = 0;
        unsigned pin_index = command == 5U ? 1 : 0;
        result = futex_prepare(task, addresses, count, operation, pin_index,
                                pin_access, keys, &pin, status);
        if (*status != KERNEL_SCHEDULER_STATUS_OK || result) return result;
        futex_lock_buckets(keys, count, resolved, guards);
        uint32_t moved = 0;
        uint32_t *word = pin_access ? (uint32_t *)((unsigned char *)pin.page +
                                     (addresses[pin_index] & BOAROS_PAGE_MASK)) : 0;
        if (command == 4U && __atomic_load_n(word, __ATOMIC_ACQUIRE) != bitset)
            result = -KERNEL_EAGAIN;
        else if (command == 5U) {
            int comparison = ((bitset >> 28) & 7U) > 4U ? -KERNEL_ENOSYS :
                             futex_atomic_op(word, bitset);
            if (comparison < 0) result = comparison;
            else {
                result = futex_wake(&keys[0], futex_wake_count(value), 0, 0,
                                    FUTEX_BITSET_MATCH_ANY, 0, resolved[0], 0);
                if (comparison) result += futex_wake(&keys[1],
                    futex_wake_count(second_count), 0, 0, FUTEX_BITSET_MATCH_ANY, 0,
                    resolved[1], 0);
            }
        } else result = futex_wake(&keys[0], requeue ? value : futex_wake_count(value),
            requeue ? second_count : 0, requeue ? &keys[1] : 0,
            command == FUTEX_WAKE_BITSET ? bitset : FUTEX_BITSET_MATCH_ANY, &moved,
            resolved[0], requeue ? resolved[1] : 0);
        futex_unlock_buckets(guards);
        if (pin_access) kernel_mm_unpin_user_page(&pin);
        while (moved--) {
            struct kernel_futex_key retired = keys[0];
            futex_key_release(&retired);
        }
        if (count == 2) futex_key_release(&keys[1]);
        futex_key_release(&keys[0]);
        return result;
    }
    return futex_wait_until(task, address, operation, value,
                            timeout_or_count != 0U, deadline_ns,
                            command == FUTEX_WAIT_BITSET
                                ? bitset : FUTEX_BITSET_MATCH_ANY,
                            status);
}

int64_t kernel_futex_restart_timed(
    struct kernel_task *task, uint64_t address, uint32_t operation,
    uint32_t value, uint64_t deadline_ns, uint32_t bitset,
    enum kernel_scheduler_status *status)
{
    uint32_t command = operation & ~(FUTEX_PRIVATE | FUTEX_CLOCK_REALTIME);

    *status = KERNEL_SCHEDULER_STATUS_OK;
    if (task != kernel_cpu_current()->current || arch_interrupt_is_enabled()) {
        *status = KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        return 0;
    }
    if ((command != 0U && command != FUTEX_WAIT_BITSET) ||
        ((operation & FUTEX_CLOCK_REALTIME) != 0U &&
         command != FUTEX_WAIT_BITSET) || bitset == 0U ||
        (address & 3U) != 0U ||
        kernel_user_range_check(address, sizeof(uint32_t)) !=
            KERNEL_UACCESS_STATUS_OK) {
        *status = KERNEL_SCHEDULER_STATUS_INVALID_STATE;
        return 0;
    }
    return futex_wait_until(task, address, operation, value, 1,
                            deadline_ns, bitset, status);
}

static void futex_wake_user(struct kernel_task *task, uint64_t address)
{
    struct kernel_futex_key key;
    struct kernel_lock_guard guards[2] = {0};
    struct futex_bucket *resolved[2];
    enum kernel_scheduler_status status = KERNEL_SCHEDULER_STATUS_OK;
    if ((address & 3U) || kernel_user_range_check(address, sizeof(uint32_t)) !=
        KERNEL_UACCESS_STATUS_OK || futex_prepare(task, &address, 1, 0, 0, 0,
            &key, 0, &status) || status != KERNEL_SCHEDULER_STATUS_OK) return;
    futex_lock_buckets(&key, 1, resolved, guards);
    (void)futex_wake(&key, 1, 0, 0, FUTEX_BITSET_MATCH_ANY, 0, resolved[0], 0);
    futex_unlock_buckets(guards);
    futex_key_release(&key);
}

void kernel_futex_release_mm(struct kernel_task *task)
{
    uint64_t address = task->clear_tid_address;
    uint32_t zero = 0U;
    size_t copied;

    task->clear_tid_address = 0U;
    if (address != 0U && kernel_mm_user_count(&task->mm) > 1U) {
        (void)kernel_copy_to_user(&task->mm, address, &zero, sizeof(zero), &copied);
        futex_wake_user(task, address);
    }
    /* 用户复制可能睡眠；返回前仍是 MM 使用者，不能让另一退出者误判为独占。 */
    (void)kernel_mm_remove_user(&task->mm);
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
                futex_wake_user(task, address);
            return 1;
        }
        if (kernel_user_cmpxchg_u32(
                &task->mm, address, value,
                (value & FUTEX_WAITERS) | FUTEX_OWNER_DIED,
                &observed) != KERNEL_UACCESS_STATUS_OK)
            return 0;
        if (observed != value) continue;
        if ((value & FUTEX_WAITERS) != 0U)
            futex_wake_user(task, address);
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
