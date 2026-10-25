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
#include <kernel/heap.h>

#include "futex_internal.h"

struct futex_bucket futex_buckets[FUTEX_BUCKETS];

/* 桶表在任务与secondary发布前初始化，热路径只取得实际桶资格。 */
void kernel_futex_init(void)
{
    for (unsigned i = 0; i < FUTEX_BUCKETS; i++) {
        kernel_wait_queue_init(&futex_buckets[i].waiters);
        kernel_mutex_init(&futex_buckets[i].operation, 26, (uintptr_t)i + 1U);
    }
}

static int futex_waiter_matches(struct kernel_wait_node *node,
    struct kernel_wait_queue *queue, const struct kernel_futex_key *key)
{
    return node->task && node->queue == queue &&
        futex_key_equal(&futex_entry(node)->key, key) && !futex_entry(node)->woken;
}

void futex_bind_entry_locked(struct kernel_wait_queue *queue,struct kernel_futex_entry *entry,
    const struct kernel_wait_token *token)
{
    entry->priority=scheduler_base_priority(token->task);
    if(kernel_wait_node_bind_locked(queue,&entry->node,token))__builtin_trap();
    if(!entry->priority)return;
    /* 与Linux futex桶一致：登记时的基础RT优先级，其余线程按FIFO。 */
    struct kernel_wait_node *at=queue->head,*node=&entry->node;
    while(at!=node && futex_entry(at)->priority>=entry->priority)at=at->next;
    if(at==node)return;
    queue->tail=node->previous;
    if(node->previous)node->previous->next=0;
    node->previous=at->previous;node->next=at;
    if(at->previous)at->previous->next=node;else queue->head=node;
    at->previous=node;
}
/* 每个排队项独立消费；超时/信号通知不等于调用者已经摘除该项。 */
static int futex_wake_entry_locked(struct kernel_wait_node *node)
{
    struct kernel_futex_entry *entry=futex_entry(node);
    if(entry->kind==FUTEX_ENTRY_REQUEUE_PI)return -KERNEL_EINVAL;
    __atomic_store_n(&entry->woken,1,__ATOMIC_RELEASE);
    kernel_wait_node_remove_locked(node);
    struct kernel_wait_token token={node->task,node->generation};
    (void)kernel_wait_notify_locked(&token,KERNEL_WAIT_WOKEN);
    return 1;
}
void kernel_futex_request_release(struct kernel_futex_request *request)
{
    if(request->task->wait.phase!=KERNEL_WAIT_FINISHED)__builtin_trap();
    /* token和节点借用已经结束；此时由执行者或独占退出清理者拥有请求。 */
    if(request->task->futex_request!=request)__builtin_trap();
    request->task->futex_request=0;
    for(unsigned i=0;i<request->count;i++)futex_key_release(&request->entries[i].key);
    if(request->heap)kernel_heap_release(request->heap,request->entries);
}
void kernel_futex_abort(struct kernel_task *task)
{ if(task->futex_request)kernel_futex_request_release(task->futex_request); }

/* 桶操作锁跨扫描保留比较/登记/迁移的次序，raw内扫描有界。
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
    int error=0;
    if (__atomic_load_n(&bucket->pi_states,__ATOMIC_RELAXED)) {
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        if (futex_pi_conflict_locked(bucket,source))return -KERNEL_EINVAL;
    }
    if (!target_bucket) {
        KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
        /* Futex只登记任务节点，没有必须锁外执行的callback。短链的节点
         * 使用不跨raw保护，无需为扫描另建游标/任务借用；长链仍分段。 */
        if (queue->registrations <= 16) {
            struct kernel_wait_node *node = queue->head;
            while (node && woken < count) {
                /* COMMITTED通知会摘除当前节点，先保存仍受保护的后继。 */
                struct kernel_wait_node *next = node->next;
                if (futex_waiter_matches(node, queue, source) &&
                    (futex_entry(node)->bitset & bitset)) {
                    int result=futex_wake_entry_locked(node);
                    if(result<0)return result;
                    woken++;
                }
                node = next;
            }
            if (moved_count) *moved_count = 0;
            return woken;
        }
    }
    kernel_wait_cursor_begin(queue, &cursor);
    while (cursor.node && (woken < count || moved < requeue)) {
        struct kernel_wait_node *node = cursor.node;
        struct kernel_task *task = node->task;
        int move = 0;
        {
            KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
            if (futex_waiter_matches(node, queue, source)) {
                if (woken < count && (futex_entry(node)->bitset & bitset)) {
                    int result=futex_wake_entry_locked(node);
                    if(result<0) { error=result;goto finished; }
                    woken++;
                } else if(moved<requeue) {
                    if(futex_entry(node)->kind==FUTEX_ENTRY_REQUEUE_PI) {
                        error=-KERNEL_EINVAL;goto finished;
                    }
                    move=1;
                }
            }
        }
        if (move) {
            /* 当前node借用覆盖task_pin，放游标后task借用阻止finish回收。
             * 同桶扫描由operation串行；此时没有其他游标持有要移动的node。 */
            kernel_wait_task_pin(task);
            kernel_wait_cursor_advance(&cursor);
            {
                KERNEL_RAW_SCOPE(guard, &kernel_wait_domain);
                if (node->queue == queue && futex_key_equal(&futex_entry(node)->key, source)) {
                    enum kernel_scheduler_status result =
                        kernel_wait_node_move_locked(node, destination);
                    if (result == KERNEL_SCHEDULER_STATUS_OK) {
                        futex_key_hold(target);
                        futex_entry(node)->key = *target;
                        moved++;
                    } else if (result != KERNEL_SCHEDULER_STATUS_EMPTY) __builtin_trap();
                }
            }
            kernel_wait_task_unpin(task);
            continue;
        }
        kernel_wait_cursor_advance(&cursor);
    }
finished:
    kernel_wait_cursor_end(&cursor);
    if (moved_count) *moved_count = moved;
    return error?error:(int64_t)woken + moved;
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
    struct kernel_futex_entry entry={.node={.task=task},.key=key,.bitset=bitset};
    struct kernel_futex_request request={.task=task,.entries=&entry,.count=1};
    {
        /* 当前任务、桶和token已由本调用取得；直接在等待保护内登记。 */
        KERNEL_RAW_SCOPE(guard,&kernel_wait_domain);
        if(task->futex_request)__builtin_trap();
        task->futex_request=&request;
        *status = kernel_wait_prepare_locked(0, deadline, 1, &request.token);
        if(!*status)futex_bind_entry_locked(&resolved[0]->waiters,&entry,&request.token);
    }
    /* token登记后不再取得任何可睡眠锁；先放桶锁再归还pin，MM编辑可排空它。 */
    futex_unlock_buckets(guards);
    kernel_mm_unpin_user_page(&pin);
    if (*status != KERNEL_SCHEDULER_STATUS_OK) {
        kernel_futex_request_release(&request);
        return 0;
    }
    enum kernel_wait_wake_reason reason;
    *status = kernel_wait_park(&request.token, &reason);
    if (kernel_wait_finish(&request.token) != KERNEL_SCHEDULER_STATUS_OK) __builtin_trap();
    int woken=entry.woken;
    kernel_futex_request_release(&request);
    if (*status != KERNEL_SCHEDULER_STATUS_OK || woken) return 0;
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
    if(command==11U)return kernel_futex_wait_requeue_pi(task,address,operation,value,timeout_or_count,address2,status);
    if(command==12U)return kernel_futex_cmp_requeue_pi(task,address,operation,value,(uint32_t)timeout_or_count,address2,bitset,status);
    if (command==6U || command==7U || command==8U || command==13U)
        return kernel_futex_pi(task,address,operation,timeout_or_count,status);
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
        struct kernel_cpu_notification_batch notifications;
        kernel_cpu_notification_batch_begin(&notifications);
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
                if (result>=0 && comparison) {
                    int64_t second=futex_wake(&keys[1],futex_wake_count(second_count),
                        0,0,FUTEX_BITSET_MATCH_ANY,0,resolved[1],0);
                    result=second<0?second:result+second;
                }
            }
        } else result = futex_wake(&keys[0], requeue ? value : futex_wake_count(value),
            requeue ? second_count : 0, requeue ? &keys[1] : 0,
            command == FUTEX_WAKE_BITSET ? bitset : FUTEX_BITSET_MATCH_ANY, &moved,
            resolved[0], requeue ? resolved[1] : 0);
        futex_unlock_buckets(guards);
        /* READY已发布；归还全部桶资格后再发IPI，远端不必等发送者退出SBI。 */
        kernel_cpu_notification_batch_end(&notifications);
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
    struct kernel_cpu_notification_batch notifications;
    kernel_cpu_notification_batch_begin(&notifications);
    (void)futex_wake(&key, 1, 0, 0, FUTEX_BITSET_MATCH_ANY, 0, resolved[0], 0);
    futex_unlock_buckets(guards);
    kernel_cpu_notification_batch_end(&notifications);
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
    __atomic_store_n(&task->robust_list_head,head,__ATOMIC_RELAXED);
}

uint64_t kernel_futex_get_robust_list(const struct kernel_task *task)
{
    return __atomic_load_n(&task->robust_list_head,__ATOMIC_RELAXED);
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

static void robust_words(struct kernel_task *task, int32_t owner_tid)
{
    uint64_t head = __atomic_exchange_n(&task->robust_list_head,0U,__ATOMIC_RELAXED);
    uint64_t next, pending, offset_bits;
    uint64_t entry, pending_entry;
    uint32_t limit = ROBUST_LIST_LIMIT;

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
        if (current != pending_entry) {
            if (entry & 1U) {
                uint64_t address;
                if (!robust_word_address(current,(int64_t)offset_bits,&address) ||
                    !kernel_futex_pi_robust(task,address,(uint32_t)owner_tid))return;
            } else if (!robust_release_word(task,current,(int64_t)offset_bits,(uint32_t)owner_tid,0))return;
        }
        entry = next;
    }
    if (pending_entry != 0U) {
        if(pending&1U) {
            uint64_t address;
            if(robust_word_address(pending_entry,(int64_t)offset_bits,&address))
                (void)kernel_futex_pi_robust(task,address,(uint32_t)owner_tid);
        } else (void)robust_release_word(task,pending_entry,(int64_t)offset_bits,(uint32_t)owner_tid,1);
    }
}
void kernel_futex_release_robust(struct kernel_task *task,int32_t owner_tid)
{
    kernel_futex_pi_cleanup_begin(task);
    robust_words(task,owner_tid);
    kernel_futex_pi_cleanup_end(task);
}
