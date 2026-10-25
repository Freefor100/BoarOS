#ifndef BOAROS_FUTEX_INTERNAL_H
#define BOAROS_FUTEX_INTERNAL_H
#include "private.h"
#include <kernel/errno.h>
#include <kernel/memory_object.h>
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

enum futex_entry_kind { FUTEX_ENTRY_WAIT, FUTEX_ENTRY_VECTOR, FUTEX_ENTRY_REQUEUE_PI };
struct kernel_futex_entry {
    struct kernel_wait_node node;
    struct kernel_futex_key key;
    uint32_t bitset, kind, index, woken, priority;
    struct kernel_futex_pi_request *pi;
    uint64_t address;
    uint32_t expected, operation;
};
struct kernel_futex_request {
    struct kernel_task *task;
    struct kernel_futex_entry *entries;
    struct kernel_heap *heap;
    struct kernel_wait_token token;
    unsigned count;
};
static inline struct kernel_futex_entry *futex_entry(struct kernel_wait_node *node)
{ return (struct kernel_futex_entry *)node; }
struct futex_bucket {
    struct kernel_mutex operation;
    struct kernel_wait_queue waiters;
    struct kernel_futex_pi_state *pi_states;
};
extern struct futex_bucket futex_buckets[FUTEX_BUCKETS];

static inline void futex_key_release(struct kernel_futex_key *key)
{
    if (key->kind == KERNEL_FUTEX_KEY_SHARED_MEMORY)
        kernel_memory_object_release(&key->shared_object);
    else if (key->kind == KERNEL_FUTEX_KEY_SHARED_FILE &&
             kernel_vfs_node_release(&key->file_node)) __builtin_trap();
    *key = (struct kernel_futex_key){0};
}

static inline void futex_key_hold(const struct kernel_futex_key *key)
{
    if (key->kind == KERNEL_FUTEX_KEY_SHARED_MEMORY &&
        kernel_memory_object_acquire(key->shared_object)) __builtin_trap();
    if (key->kind == KERNEL_FUTEX_KEY_SHARED_FILE &&
        kernel_vfs_node_acquire(key->file_node)) __builtin_trap();
}

static inline int64_t futex_prepare(struct kernel_task *task,
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

static inline int futex_key_equal(const struct kernel_futex_key *left,
                           const struct kernel_futex_key *right)
{
    return left->kind == right->kind &&
           left->identity == right->identity && left->offset == right->offset;
}

static inline struct futex_bucket *futex_bucket(const struct kernel_futex_key *key)
{
    uint64_t hash = (key->identity >> 4U) ^ key->identity ^
                    (key->offset >> 2U) ^ (key->offset >> 12U) ^ key->kind;
    unsigned index = hash & (FUTEX_BUCKETS - 1U);
    return &futex_buckets[index];
}

static inline void futex_lock_buckets(const struct kernel_futex_key *keys,
    unsigned count, struct futex_bucket **resolved, struct kernel_lock_guard *guards)
{
    struct futex_bucket *first = resolved[0] = futex_bucket(&keys[0]);
    struct futex_bucket *second = resolved[1] = count == 2 ? futex_bucket(&keys[1]) : first;
    if (first > second) {
        struct futex_bucket *swap = first; first = second; second = swap;
    }
    kernel_mutex_lock(&first->operation, &guards[0]);
    if (first != second) kernel_mutex_lock(&second->operation, &guards[1]);
}

static inline void futex_unlock_buckets(struct kernel_lock_guard *guards)
{
    if (guards[1].lock) kernel_lock_release(&guards[1]);
    kernel_lock_release(&guards[0]);
}


struct kernel_futex_pi_request;
struct kernel_futex_pi_state {
    struct kernel_futex_key key;
    struct kernel_heap *heap;
    struct futex_bucket *bucket;
    struct kernel_futex_pi_state *hash_next, *owner_previous, *owner_next;
    struct kernel_task *owner;
    struct kernel_futex_pi_request *head, *tail;
    uint32_t references, attached, word_tid, needs_fixup;
};
struct kernel_futex_pi_request {
    struct kernel_futex_pi_request *previous, *next;
    struct kernel_task *task;
    struct kernel_futex_pi_state *state, *candidate;
    struct kernel_task *exiting;
    struct kernel_futex_key key;
    struct kernel_mm_user_pin pin, source_pin;
    struct kernel_wait_token token;
    uint64_t address, order;
    uint32_t operation, queued, granted, requeue_stage;
};
void futex_bind_entry_locked(struct kernel_wait_queue *,struct kernel_futex_entry *,const struct kernel_wait_token *);
void kernel_futex_request_release(struct kernel_futex_request *);
int64_t kernel_futex_pi(struct kernel_task *,uint64_t,uint32_t,uint64_t,
    enum kernel_scheduler_status *);
int futex_pi_conflict_locked(struct futex_bucket *,const struct kernel_futex_key *);
void kernel_futex_pi_abort(struct kernel_task *);
int64_t kernel_futex_wait_requeue_pi(struct kernel_task *,uint64_t,uint32_t,uint32_t,uint64_t,uint64_t,enum kernel_scheduler_status *);
int64_t kernel_futex_cmp_requeue_pi(struct kernel_task *,uint64_t,uint32_t,uint32_t,uint32_t,uint64_t,uint32_t,enum kernel_scheduler_status *);
void kernel_futex_pi_cleanup_begin(struct kernel_task *);
void kernel_futex_pi_cleanup_end(struct kernel_task *);
int kernel_futex_pi_robust(struct kernel_task *,uint64_t,uint32_t);
#endif
